#include "audio-hook-info.h"
#include "win-pipe/win-pipe.h"

#include <string>

#include <audioclient.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <type_traits>
#include <windows.h>

#include <media-io/audio-io.h>
#include <util/platform.h>

win_pipe::sender g_sender;
std::vector<BYTE> g_buffer;

// clang-format off

HRESULT (WINAPI* g_original_get_buffer)(IUnknown*, UINT32, BYTE**) = nullptr;

HRESULT (WINAPI* g_original_release_buffer)(IUnknown*, UINT32,DWORD) = nullptr;

HRESULT (WINAPI* g_original_initialize)(IUnknown*, AUDCLNT_SHAREMODE, DWORD,
    REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID) = nullptr;

// clang-format on

IAudioRenderClient* g_audio_render_client = nullptr;
IAudioClient* g_audio_client = nullptr;
IAudioClock* g_audio_clock = nullptr;
WAVEFORMATEX* g_wave_format = nullptr;
BYTE* g_data = nullptr;

void init_pipe()
{
    g_sender = win_pipe::sender(AUDIO_PIPE_NAME);
}

template <typename T> inline void safe_release(T** out_COM_obj)
{
    static_assert(std::is_base_of<IUnknown, T>::value,
        "Object must implement IUnknown");
    if (*out_COM_obj)
        (*out_COM_obj)->Release();
    *out_COM_obj = nullptr;
}

bool hook_COM(IUnknown* COM_obj, void* hook_func,
    void** out_original_method, size_t offset)
{
    if (!COM_obj)
        return false;

    void** vtable = *((void***)COM_obj);
    if (vtable[offset] == hook_func)
        return false;

    unsigned long old_protect = 0;
    if (!VirtualProtect(*(void**)(COM_obj), sizeof(void*),
            PAGE_EXECUTE_READWRITE, &old_protect))
        return false;

    if (out_original_method)
        *out_original_method = vtable[offset];
    vtable[offset] = hook_func;

    return true;
}

HRESULT WINAPI get_buffer_hook(IUnknown* This, UINT32 NumFramesRequested,
    BYTE** ppData)
{
    HRESULT ret = g_original_get_buffer(This, NumFramesRequested, ppData);
    g_data = *ppData;
    return ret;
}

HRESULT WINAPI release_buffer_hook(IUnknown* This, UINT32 NumFramesWritten,
    DWORD dwFlags)
{
    HRESULT ret = g_original_release_buffer(This, NumFramesWritten, dwFlags);
    if (NumFramesWritten == 0)
        return ret;

    DWORD md_size = sizeof(pipe_metadata);
    DWORD data_size = NumFramesWritten * g_wave_format->nBlockAlign;
    DWORD buffer_size = md_size + data_size;
    
    g_buffer.reserve(buffer_size);
    pipe_metadata* md = (pipe_metadata*)g_buffer.data();
    BYTE* data = g_buffer.data() + md_size;

    uint64_t position, qpcPosition = 0;
    g_audio_clock->GetPosition(&position, &qpcPosition);
    md->timestamp = position * 100;

    memcpy(data, g_data, data_size);

    g_sender.send(g_buffer.data(), buffer_size);

    return ret;
}

HRESULT WINAPI initialize_hook(IUnknown* This, AUDCLNT_SHAREMODE ShareMode,
    DWORD StreamFlags, REFERENCE_TIME hnsBufferDuration,
    REFERENCE_TIME hnsPeriodicity, const WAVEFORMATEX* pFormat,
    LPCGUID AudioSessionGuid)
{
    *g_wave_format = *pFormat;
    return g_original_initialize(This, ShareMode, StreamFlags,
        hnsBufferDuration, hnsPeriodicity, pFormat,
        AudioSessionGuid);
}

bool core_audio_hook()
{
    bool success = false;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
        return success;

    IMMDeviceEnumerator* device_enum;
    IMMDevice* device;

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&device_enum)))
        goto exit_uninitialize;

    if (FAILED(device_enum->GetDefaultAudioEndpoint(eRender, eMultimedia,
            &device)))
        goto exit_release_enum;

    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL,
            (void**)&g_audio_client)))
        goto exit_release_device;

    if (FAILED(g_audio_client->GetMixFormat(&g_wave_format)))
        goto exit_release_device;

    if (FAILED(g_audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
            10'000'000LL, 0, g_wave_format, NULL)))
        goto exit_release_device;

    if (FAILED(g_audio_client->GetService(__uuidof(IAudioRenderClient),
            (void**)&g_audio_render_client)))
        goto exit_release_device;

    if (FAILED(g_audio_client->GetService(__uuidof(IAudioClock),
            (void**)&g_audio_clock)))
        goto exit_release_device;

    success = true;
    hook_COM(g_audio_render_client, &get_buffer_hook,
        (void**)&g_original_get_buffer, 3);
    hook_COM(g_audio_render_client, &release_buffer_hook,
        (void**)&g_original_release_buffer, 4);
    hook_COM(g_audio_client, *initialize_hook,
        (void**)&g_original_initialize, 3);
    init_pipe();

exit_release_device:
    safe_release(&device);
exit_release_enum:
    safe_release(&device_enum);
exit_uninitialize:
    CoUninitialize();
    return success;
}

void core_audio_unhook()
{
    hook_COM(g_audio_render_client, g_original_get_buffer, nullptr, 3);
    hook_COM(g_audio_render_client, g_original_release_buffer, nullptr, 4);
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        safe_release(&g_audio_render_client);
        safe_release(&g_audio_client);
        safe_release(&g_audio_clock);
        CoTaskMemFree(g_wave_format);
        CoUninitialize();
    }
}
