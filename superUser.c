/*
	superUser

	A simple and lightweight utility to start any process
	with TrustedInstaller privileges.

	https://github.com/mspaintmsi/superUser

	superUser.c

*/

#include <windows.h>
#include <stdio.h>

#include "tokens.h" // Defines tokens and privileges management functions

// Program options
static struct {
	unsigned int bReturnCode : 1;  // Whether to return process exit code
	unsigned int bSeamless : 1;    // Whether child process shares parent's console
	unsigned int bVerbose : 1;     // Whether to print debug messages or not
	unsigned int bWait : 1;        // Whether to wait to finish created process
} options = {0};

#define wputs _putws
#define wprintfv(...) \
if (options.bVerbose) wprintf(__VA_ARGS__); // Only use when bVerbose in scope

/*
	Return codes (without /r option):
		1 - Invalid argument
		2 - Failed to acquire SeDebugPrivilege
		3 - Failed to open/start TrustedInstaller process/service
		4 - Process creation failed
		5 - Another fatal error occurred

	If /r option is specified, exit code of the child process is returned.
	If superUser fails, it returns the code -(EXIT_CODE_BASE + errCode),
	where errCode is one of the codes listed above.
*/

#define EXIT_CODE_BASE 1000000
static int nChildExitCode = 0;


static int createTrustedInstallerProcess( wchar_t* pwszImageName )
{
	int errCode = 0;
	HANDLE hTIProcess = NULL, hTIToken = NULL;

	// Start the TrustedInstaller service and get its process handle
	errCode = getTrustedInstallerProcess( &hTIProcess );
	if (errCode) return errCode;

	if (options.bSeamless) {
		// Get the TrustedInstaller process token
		errCode = getTrustedInstallerToken( hTIProcess, &hTIToken );
		if (errCode) {
			CloseHandle( hTIProcess );
			return errCode;
		}

		// Get the console session id and set it in the token
		DWORD dwSessionId = WTSGetActiveConsoleSessionId();
		if (dwSessionId != (DWORD) -1) {
			SetTokenInformation( hTIToken, TokenSessionId, (PVOID) &dwSessionId,
				sizeof( DWORD ) );
		}

		// Set all privileges in the child process token
		setAllPrivileges( hTIToken, options.bVerbose );
	}

	// Initialize startupInfo

	STARTUPINFOEX startupInfo = {0};

	startupInfo.StartupInfo.cb = sizeof( STARTUPINFOEX );
	startupInfo.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
	startupInfo.StartupInfo.wShowWindow = SW_SHOWNORMAL;

	if (! options.bSeamless) {
		// Initialize attribute lists for "parent assignment"

		SIZE_T attributeListLength = 0;
		InitializeProcThreadAttributeList( NULL, 1, 0, (PSIZE_T) &attributeListLength );
		startupInfo.lpAttributeList = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
			attributeListLength );
		InitializeProcThreadAttributeList( startupInfo.lpAttributeList, 1, 0,
			(PSIZE_T) &attributeListLength );

		UpdateProcThreadAttribute( startupInfo.lpAttributeList, 0,
			PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &hTIProcess, sizeof( HANDLE ), NULL, NULL );
	}

	// Create process

	PROCESS_INFORMATION processInfo = {0};
	DWORD dwCreationFlags = 0;
	if (! options.bSeamless)
		dwCreationFlags = CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
		CREATE_NEW_CONSOLE;

	wprintfv( L"[D] Creating specified process\n" );

	BOOL bCreateResult = CreateProcessAsUser(
		hTIToken,
		NULL,
		pwszImageName,
		NULL,
		NULL,
		FALSE,
		dwCreationFlags,
		NULL,
		NULL,
		(LPSTARTUPINFO) &startupInfo,
		&processInfo
	);

	DWORD dwCreateError = bCreateResult ? 0 : GetLastError();

	if (options.bSeamless) CloseHandle( hTIToken );
	else {
		DeleteProcThreadAttributeList( startupInfo.lpAttributeList );
		HeapFree( GetProcessHeap(), 0, startupInfo.lpAttributeList );
	}
	CloseHandle( hTIProcess );

	if (bCreateResult) {
		if (! options.bSeamless) {
			HANDLE hProcessToken = NULL;
			OpenProcessToken( processInfo.hProcess, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
				&hProcessToken );
			// Set all privileges in the child process token
			setAllPrivileges( hProcessToken, options.bVerbose );
			CloseHandle( hProcessToken );

			ResumeThread( processInfo.hThread );
		}

		wprintfv( L"[D] Created process ID: %ld\n", processInfo.dwProcessId );

		if (options.bWait) {
			wprintfv( L"[D] Waiting for process to exit\n" );
			WaitForSingleObject( processInfo.hProcess, INFINITE );
			wprintfv( L"[D] Process exited\n" );

			// Get the child's exit code
			DWORD dwExitCode;
			if (GetExitCodeProcess( processInfo.hProcess, &dwExitCode )) {
				nChildExitCode = dwExitCode;
				wprintfv( L"[D] Process exit code: %ld\n", dwExitCode );
			}
		}

		CloseHandle( processInfo.hProcess );
		CloseHandle( processInfo.hThread );
	}
	else {
		// Most commonly - 0x2 - The system cannot find the file specified.
		printError( L"Process creation failed", dwCreateError, 0 );
		return 4;
	}

	return 0;
}


static BOOL getArgument( wchar_t** ppArgument, wchar_t** ppArgumentIndex )
{
	// Current pointer to the remainder of the line to be parsed.
	// Initialized with the full command line on the first call.
	static wchar_t* p = NULL;
	if (! p) {
		p = GetCommandLine();

		// Skip program name
		BOOL bQuote = FALSE;
		while (*p) {
			if (*p == L'"') bQuote = ! bQuote;
			else if (! bQuote && (*p == L' ' || *p == L'\t')) break;
			p++;
		}
	}

	// Free the previous argument (if it exists)
	if (*ppArgument) HeapFree( GetProcessHeap(), 0, *ppArgument );
	*ppArgument = NULL;

	// Search argument

	// Skip spaces
	while (*p == L' ' || *p == L'\t') p++;

	if (*p) {
		// Argument found
		wchar_t* pBegin = p;

		// Search the end of the argument
		while (*p && *p != L' ' && *p != L'\t') p++;

		size_t nArgSize = (p - pBegin) * sizeof( wchar_t );
		*ppArgument = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
			nArgSize + sizeof( wchar_t ) );
		memcpy( *ppArgument, pBegin, nArgSize );
		*ppArgumentIndex = pBegin;
		return TRUE;
	}

	// Argument not found
	return FALSE;
}


static int getExitCode( int code )
{
	if (code == -1) code = 0;  // Print help, exit with code 0
	if (options.bReturnCode) {
		if (code) code = -(EXIT_CODE_BASE + code);
		else code = nChildExitCode;
	}
	return code;
}


static void printHelp( void )
{
	wputs(
		L"superUser.exe [options] [command_to_run]\n\
Options: (You can use either '-' or '/')\n\
  /h - Display this help message.\n\
  /r - Return exit code of child process. Requires /w.\n\
  /s - Child process shares parent's console. Requires /w.\n\
  /v - Display verbose messages.\n\
  /w - Wait for the created process to finish before exiting." );
}


int wmain( int argc, wchar_t* argv[] )
{
	int errCode = 0;  // superUser error code

	// Command to run (executable file name of process to create, followed by
	// parameters) - basically the first non-option argument or "cmd.exe".
	wchar_t* pwszCommandLine = NULL;

	wchar_t* pwszArgument = NULL;  // Command line argument
	wchar_t* pwszArgumentIndex = NULL;  // Pointer to argument in command line

	// Parse command line options

	while (getArgument( &pwszArgument, &pwszArgumentIndex )) {
		// Check for an at-least-two-character string beginning with '/' or '-'
		if ((*pwszArgument == L'/' || *pwszArgument == L'-') && pwszArgument[ 1 ]) {
			int j = 1;
			wchar_t opt;
			while ((opt = pwszArgument[ j ])) {
				// Multiple options can be grouped together (eg: /wrs)
				switch (opt) {
				case 'h':
					printHelp();
					errCode = -1;
					goto done_params;
				case 'r':
					options.bReturnCode = 1;
					break;
				case 's':
					options.bSeamless = 1;
					break;
				case 'v':
					options.bVerbose = 1;
					break;
				case 'w':
					options.bWait = 1;
					break;
				default:
					printError( L"Invalid option", 0, 0 );
					errCode = 1;
					goto done_params;
				}
				j++;
			}
		}
		else {
			// First non-option argument found
			pwszCommandLine = pwszArgumentIndex;
			break;
		}
	}
done_params:
	// Free the last argument (if it exists)
	if (pwszArgument) HeapFree( GetProcessHeap(), 0, pwszArgument );

	if (errCode) return getExitCode( errCode );

	// Check the consistency of the options
	if ((options.bReturnCode || options.bSeamless) && ! options.bWait) {
		printError( L"/r or /s option requires /w", 0, 0 );
		return getExitCode( 1 );
	}

	if (! pwszCommandLine) pwszCommandLine = L"cmd.exe";

	wprintfv( L"[D] Your commandline is \"%ls\"\n", pwszCommandLine );

	// pwszCommandLine may be read-only. It must be copied to a writable area.
	size_t nCommandLineBufSize = (wcslen( pwszCommandLine ) + 1) * sizeof( wchar_t );
	wchar_t* pwszImageName = HeapAlloc( GetProcessHeap(), 0, nCommandLineBufSize );
	memcpy( pwszImageName, pwszCommandLine, nCommandLineBufSize );

	errCode = acquireSeDebugPrivilege();
	if (! errCode && options.bSeamless) errCode = createSystemContext();
	if (! errCode) errCode = createTrustedInstallerProcess( pwszImageName );

	HeapFree( GetProcessHeap(), 0, pwszImageName );

	return getExitCode( errCode );
}
