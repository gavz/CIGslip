// Inject.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

extern "C" {
	extern void xfunc();
	extern void xxend();
	extern void* xapi;
	extern UNICODE_STRING	xdll;
	
	extern DWORD xtid;

	extern void* xsect_cr_api;
	extern void* xsect_handle[];
	extern void* xsio_api;

}

typedef NTSTATUS(NTAPI *NtCreateSection)(
	_Out_    PHANDLE            SectionHandle,
	_In_     ACCESS_MASK        DesiredAccess,
	_In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
	_In_opt_ PLARGE_INTEGER     MaximumSize,
	_In_     ULONG              SectionPageProtection,
	_In_     ULONG              AllocationAttributes,
	_In_opt_ HANDLE             FileHandle
	);

HANDLE make_section(wchar_t* dll) {

	HANDLE hExe = CreateFileW(dll,
		GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == hExe)
	{
		printf("Error %d in CreateFileW\n", GetLastError());
		return 0;
	}

	NtCreateSection createSection = (NtCreateSection)GetProcAddress(GetModuleHandleA("ntdll"), "NtCreateSection");

	HANDLE hSection;
	NTSTATUS ret = createSection(&hSection, SECTION_ALL_ACCESS, NULL, 0, PAGE_EXECUTE, SEC_IMAGE, hExe);
	if (FALSE == NT_SUCCESS(ret))
	{
		printf("Error %#x in createSection\n", ret);
		return 0;
	}

	xsect_cr_api = createSection;

	return hSection;
}


bool prepare_create(HANDLE process, HANDLE section) {
	for (int i = 0; i < 10; ++i) {
		void** addr = &xsect_handle[i];
		if (!DuplicateHandle(GetCurrentProcess(), section, process, addr, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
			printf("Error %d duplicating handle at %#p\n", GetLastError(), addr);
		}
	}

	DWORD old;
	BOOL b = VirtualProtectEx(process, (char*)xsect_cr_api - 8, 0x100, PAGE_EXECUTE_READWRITE, &old);
	if (!b) {
		printf("Error2 %d in VP\n", GetLastError());
		return false;
	}

	return true;
}


void inject(HANDLE process, wchar_t* dll) {

	auto s = (char*)VirtualAllocEx(process, 0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!s) {
		printf("Error %d in alloc\n", GetLastError());
		return;
	}

	xapi = GetProcAddress(GetModuleHandleA("ntdll"), "LdrLoadDll");
	if (!xapi) {
		printf("Error %d in GPA\n", GetLastError());
		return;
	}

	xdll.Length = (USHORT)wcslen(dll) * 2;
	xdll.MaximumLength = xdll.Length + sizeof(dll[0]);
	xdll.Buffer = PWSTR(((char*)(&xdll + 1) - (char*)xfunc) + s);
	memcpy(&xdll + 1, dll, xdll.MaximumLength);

	SIZE_T written;
	if (!WriteProcessMemory(process, s, &xfunc, (char*)&xxend - (char*)&xfunc, &written)) {
		printf("Error %d in WPM\n", GetLastError());
		return;
	}

	DWORD tid;
	HANDLE thread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)s, 0, CREATE_SUSPENDED, &tid);
	if (!thread) {
		printf("Error %d in CreateRemoteThread\n", GetLastError());
		return;
	}

	xtid = tid;

	// write again to update TID
	if (!WriteProcessMemory(process, s, &xfunc, (char*)&xxend - (char*)&xfunc, &written)) {
		printf("Error %d in WPM2\n", GetLastError());
		return;
	}

	printf("Thread %d(%#x) created at %#p\n", tid, tid, s);

	printf("Press <Enter> to start...");
	getchar();

	ResumeThread(thread);

	void* p;
	while (true) {
		SIZE_T read;
		if (!ReadProcessMemory(process, s + ((char*)&xapi - (char*)xfunc), &p, sizeof(p), &read))
			break;

		if (p != xapi) {
			printf("Operation status %#p\n", p);
			break;
		}

		Sleep(100);
	}

}

int LoadPrivilege(const TCHAR* privilege) {
	HANDLE hToken;
	LUID Value;
	TOKEN_PRIVILEGES tp;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
		return(GetLastError());
	if (!LookupPrivilegeValue(NULL, privilege, &Value))
		return(GetLastError());
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = Value;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL))
		return(GetLastError());
	CloseHandle(hToken);
	return 1;
}




int wmain(/*int argc, wchar_t** argv*/)
{
	int argc;
	// shell api processes quoted arguments
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argc < 3) {
		printf("Use:\n");
		printf("%ws <PID> <DLL path>\n", argv[0]);
		return 0;
	}
	if (LoadPrivilege(SE_DEBUG_NAME))
		printf("Got DEBUG priviledge to open process...\n");
	else
		printf("Failure obtaining DEBUG priviledge... ");


	DWORD pid = _wtol(argv[1]);
	HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);

	if (!process) {
		printf("Error %d opening process %d\n", GetLastError(), pid);
		return 0;
	}

	{

		typedef struct _OBJECT_HANDLE_ATTRIBUTE_INFORMATION
		{
			BOOLEAN Inherit;
			BOOLEAN ProtectFromClose;
		} OBJECT_HANDLE_ATTRIBUTE_INFORMATION, *POBJECT_HANDLE_ATTRIBUTE_INFORMATION;


		typedef NTSTATUS(NTAPI *NtSetInformationObject)(IN HANDLE 	ObjectHandle,
			IN int/*OBJECT_INFORMATION_CLASS*/ 	ObjectInformationClass,
			IN PVOID 	ObjectInformation,
			IN ULONG 	Length
			);

		OBJECT_HANDLE_ATTRIBUTE_INFORMATION HandleInfo = { FALSE, TRUE };

		auto ntSetInformationObject = (NtSetInformationObject)GetProcAddress(GetModuleHandleA("ntdll"), "NtSetInformationObject");
		xsio_api = ntSetInformationObject;
	}


	HANDLE section = make_section(argv[2]);
	prepare_create(process, section);
	inject(process, argv[2]);
}

