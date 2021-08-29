#include <Windows.h>
#include <stdbool.h>
#include <stdio.h>

extern bool core_audio_hook();
extern void core_audio_unhook();

#define HOOK_NAME L"audio_hook_dup_mutex"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        core_audio_hook();
    } else if (reason == DLL_PROCESS_DETACH) {
        core_audio_unhook();
    }

    return true;
}
