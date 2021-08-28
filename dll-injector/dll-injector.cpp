#include <Windows.h>

int main(int argc, char *argv[])
{
	if (argc <= 2)
		return EXIT_FAILURE;

	const char* dll_path = argv[1];
	DWORD target_pid = atol(argv[2]);

	HMODULE kernel32dll = GetModuleHandleA("kernel32.dll");
	if (!kernel32dll)
		return EXIT_FAILURE;

	HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, target_pid);
	if (!process)
		goto release_kernel32dll;

	HANDLE load_library_param =
		VirtualAllocEx(process, NULL, strlen(dll_path),
			       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!load_library_param)
		goto release_process;

	WriteProcessMemory(process, load_library_param, dll_path,
			   strlen(dll_path), NULL);

	auto load_library_func = (LPTHREAD_START_ROUTINE)GetProcAddress(
		kernel32dll, "LoadLibraryA");
	HANDLE thread = CreateRemoteThread(process, NULL, NULL,
					   load_library_func,
					   load_library_param, NULL, NULL);
	if (!thread)
		goto release_param;
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);

release_param:
	VirtualFreeEx(process, load_library_param, NULL, MEM_RELEASE);
release_process:
	CloseHandle(process);
release_kernel32dll:
	CloseHandle(kernel32dll);
	return EXIT_SUCCESS;
}
