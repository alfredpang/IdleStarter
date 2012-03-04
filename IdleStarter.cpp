//
// Copyright (c) 2012, Alfred Pang
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// Neither the name of the organization nor the names of its contributors may
// be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//

#include <iostream>
#include <fstream>
#include <windows.h>
#include <time.h>
#include <comutil.h>

#ifdef _DEBUG
# pragma comment(lib, "comsuppwd.lib")
#else
# pragma comment(lib, "comsuppw.lib")
#endif

using namespace std;

_bstr_t bUsageText = "\
 [-help] [-run \"<command>\"] [-wait <minutes>] [-cpu <percent>] [-memory <percent>]\n\
\n\
where:\n\
	-help = Shows usage\n\
	-run \"<command>\" = Command to run\n\
	-wait <minutes> = Waits minutes before starting (default 1)\n\
	-cpu <percent> = Run only if cpu utilization is less than this (default is 100)\n\
	-memory <percent>  = Run only if memory utilization is less than this (default is 100)\n\
";

// Secret arguments:
// [-x <logfile>]      = Log debug messages here
// Future:
// [-once]             = Only attempt to run once


////////////////////////////////////////////////////////////////////////
// Global Constants
#define POLL_TIMER_ID 100008

// Globals
#define RUNNERMODE_WAIT_FOR_IDLE  0
#define RUNNERMODE_RUNNING        1

int  runnerMode = RUNNERMODE_WAIT_FOR_IDLE;
BOOL hasHumanActivity = TRUE;  // human moved since last look
DWORD currentTick;             // milliseconds
HANDLE hProcess = NULL;		   // program

bool firstTimeGetSystemTimes = true;
ULARGE_INTEGER prevIdle;
ULARGE_INTEGER prevKernel;
ULARGE_INTEGER prevUser;
unsigned cpuUtilization = 100;

#define FT2UL(filetime, ularge) {ularge.LowPart = filetime.dwLowDateTime; ularge.HighPart = filetime.dwHighDateTime;}

// User argument globals
unsigned minutesToWait = 1;
unsigned cpuPercentThreshold = 100;
unsigned memoryPercentThreshold = 100;
_bstr_t  commandToRun;

// Logging
struct nullstream: ostream {
	nullstream(): ostream(0) {}
};
nullstream nologging;
ofstream * dologging = NULL;
ostream * pLogStream = &nologging;
#define log logTime(); (*pLogStream)

void logTime()
{
	char tmp[128];
	_strtime_s( tmp, sizeof(tmp) );
	(*pLogStream) << tmp << " ";
}

void updateCpuUtilCalculation()
{
	FILETIME currIdleTime;
	FILETIME currKernelTime;
	FILETIME currUserTime;

	if (firstTimeGetSystemTimes) {
		if (!GetSystemTimes(&currIdleTime, &currKernelTime, &currUserTime)) {
			log << "GetSystemTimes failed. Error=" << GetLastError() << endl;
			return;
		}
		FT2UL(currIdleTime, prevIdle);
		FT2UL(currKernelTime, prevKernel);
		FT2UL(currUserTime, prevUser);
		firstTimeGetSystemTimes = false;
		return;
	}

	if (!GetSystemTimes(&currIdleTime, &currKernelTime, &currUserTime)) {
		log << "GetSystemTimes failed. Error=" << GetLastError() << endl;
		return;
	}

	// calculate
	ULARGE_INTEGER cIdle, cKernel, cUser;
	FT2UL(currIdleTime, cIdle);
	FT2UL(currKernelTime, cKernel);
	FT2UL(currUserTime, cUser);

	ULARGE_INTEGER totalTime;
	totalTime.QuadPart = (cKernel.QuadPart - prevKernel.QuadPart)
					   + (cUser.QuadPart - prevUser.QuadPart);

	cpuUtilization = (unsigned)(
		(totalTime.QuadPart - (cIdle.QuadPart - prevIdle.QuadPart)) * 100
		/ totalTime.QuadPart);

	prevIdle.QuadPart = cIdle.QuadPart;
	prevKernel.QuadPart = cKernel.QuadPart;
	prevUser.QuadPart = cUser.QuadPart;

	//log << "Current CPU utilization % = " << cpuUtilization << endl;
}

bool thresholdConditionsMet()
{
	MEMORYSTATUSEX memoryStatus;

	memoryStatus.dwLength = sizeof(MEMORYSTATUSEX);

	if (!GlobalMemoryStatusEx(&memoryStatus)) {
		log << "GlobalMemoryStatusEx returned with error " << GetLastError() << endl;
		log << memoryStatus.dwMemoryLoad << endl;
		return false;
	}

	if (memoryStatus.dwMemoryLoad > memoryPercentThreshold) {
		log << "Not ready to run. MemoryLoad > Threshold. "
			<< memoryStatus.dwMemoryLoad << " > " << memoryPercentThreshold << endl;
		return false;
	}

	log << "MemoryLoad < Threshold. "
		<< memoryStatus.dwMemoryLoad << " <= " << memoryPercentThreshold << endl;

	if (cpuUtilization > cpuPercentThreshold) {
		log << "Not ready to run. CpuLoad > Threshold. "
			<< cpuUtilization << " > " << cpuPercentThreshold << endl;
		return false;
	}

	log << "CpuLoad < Threshold. "
		<< cpuUtilization << " > " << cpuPercentThreshold << endl;

	return true;
}

bool runCommand()
{
	STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );

	if( !CreateProcess( NULL,   // No module name (use command line)
        commandToRun,        // Command line
        NULL,           // Process handle not inheritable
        NULL,           // Thread handle not inheritable
        FALSE,          // Set handle inheritance to FALSE
        0,              // No creation flags
        NULL,           // Use parent's environment block
        NULL,           // Use parent's starting directory 
        &si,            // Pointer to STARTUPINFO structure
        &pi )           // Pointer to PROCESS_INFORMATION structure
    ) 
    {
        log << "CreateProcess failed " << GetLastError() << endl;
		hProcess = NULL;
		return false;
    }

	log << "CreateProcess succeeded. processid=" << pi.dwProcessId << endl;
	hProcess = pi.hProcess;
	return true;
}

LRESULT CALLBACK keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	PKBDLLHOOKSTRUCT p = (PKBDLLHOOKSTRUCT) (lParam);
	if (wParam == WM_KEYDOWN) {
		hasHumanActivity = TRUE;
	}
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	hasHumanActivity = TRUE;
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

void CALLBACK timerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	updateCpuUtilCalculation();
	thresholdConditionsMet();

	switch (runnerMode) {
	case RUNNERMODE_WAIT_FOR_IDLE:
		if (hasHumanActivity) {
			// oh reset
			log << "oh reset tick count" << endl;
			currentTick = GetTickCount();
		}
		else {
			DWORD diffTick = GetTickCount() - currentTick;
			log << "diff tick " << diffTick << endl;
			if (diffTick > (minutesToWait * 60 * 1000) && thresholdConditionsMet()) {
				// run it
				if (!runCommand()) {
					log << "Unable to run command? Try again in a bit." << endl;
					currentTick = GetTickCount();
				}
				else {
					log << "Command started." << endl;
					runnerMode = RUNNERMODE_RUNNING;
				}
			}
		}
		break;

	case RUNNERMODE_RUNNING:
		// future, consider monitoring and restarting as appropriate
		// unless we are running just once
		if (hProcess == NULL) {
			log << "Process no longer running. Go back to idle wait." << endl;
			runnerMode = RUNNERMODE_WAIT_FOR_IDLE;
			currentTick = GetTickCount();
			break;
		}

		{
		DWORD ret = WaitForSingleObjectEx(hProcess, 0, TRUE);
		if (ret != WAIT_TIMEOUT) {
			// probably stopped
			log << "Process no longer running. Go back to idle wait." << endl;
			runnerMode = RUNNERMODE_WAIT_FOR_IDLE;
			currentTick = GetTickCount();
		}
		}
		break;
	}

	hasHumanActivity = FALSE;
}

void usage(LPWSTR commandName)
{
	_bstr_t bCommandName(commandName);
	MessageBox(NULL, bCommandName + bUsageText, L"Usage", MB_OK);
	exit(0);
}

void parseArgs()
{
	int numArgs;
	LPWSTR * args = CommandLineToArgvW(GetCommandLineW(), &numArgs);
	int currentArg = 0;
	bool commandSpecified = false;

	if (numArgs == 1) {
		usage(args[0]);
	}

	while (++currentArg < numArgs) {
		_bstr_t bOption(args[currentArg]);
		char * option = (char *)bOption;
		if (option[0] != '-' || option[1] == 'h') {
			usage(args[0]);
		}

		switch (option[1]) {
		case 'r':
			if (currentArg >= numArgs) usage(args[0]);
			commandToRun = args[++currentArg];
			commandSpecified = true;
			break;

		case 'w':
			{
			if (currentArg >= numArgs) usage(args[0]);
			_bstr_t value(args[++currentArg]);
			minutesToWait = atoi((char *)value);
			minutesToWait = (minutesToWait <= 0)? 1: minutesToWait;
			break;
			}

		case 'c':
			{
			if (currentArg >= numArgs) usage(args[0]);
			_bstr_t value(args[++currentArg]);
			cpuPercentThreshold = atoi((char *)value);
			cpuPercentThreshold = (cpuPercentThreshold <= 0)? 0: cpuPercentThreshold;
			break;
			}
		case 'm':
			{
			if (currentArg >= numArgs) usage(args[0]);
			_bstr_t value(args[++currentArg]);
			memoryPercentThreshold = atoi((char *)value);
			memoryPercentThreshold = (memoryPercentThreshold <= 0)? 0: memoryPercentThreshold;
			break;
			}
		case 'x':
			{
			if (currentArg >= numArgs) usage(args[0]);
			_bstr_t value(args[++currentArg]);
			dologging = new ofstream((const wchar_t *)value, ios::out);
			pLogStream = dologging;
			break;
			}
		}
	}

	if (!commandSpecified) {
		log << "Command not specified." << endl;
		usage(args[0]);
	}

	log << "Arguments parsed:" << endl;
	log << "-run " << commandToRun << endl;
	log << "-wait " << minutesToWait << endl;
	log << "-cpu " << cpuPercentThreshold << endl;
	log << "-memory " << memoryPercentThreshold << endl;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
	parseArgs();

	// Set windows hook
	HHOOK keyboardHook = SetWindowsHookEx(
		WH_KEYBOARD_LL,
		keyboardHookProc,
		hInstance,
		0);

	HHOOK mouseHook = SetWindowsHookEx(
		WH_MOUSE_LL,
		mouseHookProc,
		hInstance,
		0);

	// Set timer
	currentTick = GetTickCount();
	SetTimer(NULL, POLL_TIMER_ID, 2000, timerProc);

	MSG msg;
    while(GetMessage(&msg, NULL, 0, 0) > 0)
    {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// need to catch Control-C so we can clean up
	if (dologging) {
		delete dologging;
	}

	return msg.wParam;
}


