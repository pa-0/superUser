#pragma once
/*
	https://github.com/mspaintmsi/superUser

	tokens.h

	Tokens and privileges management functions

*/

int acquireSeDebugPrivilege( void );
int createChildProcessToken( HANDLE hBaseProcess, HANDLE* phNewToken );
int createSystemContext( void );
int getTrustedInstallerProcess( HANDLE* phTIProcess );
void setAllPrivileges( HANDLE hToken, BOOL bVerbose );
