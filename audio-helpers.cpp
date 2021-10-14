#include "audio-helpers.h"

#include <functional>

#include <Psapi.h>
#include <Windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <mmeapi.h>

#include <obs-module.h>
#include <util/platform.h>

//--------------------------------------------------------------------[ file out

HANDLE create_file(const char* path)
{
#if _DEBUG
    return CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
#endif
}

void write_file(HANDLE file, const void* buffer, DWORD size)
{
#if _DEBUG
    WriteFile(file, buffer, size, NULL, NULL);
#endif
}

void free_file(HANDLE file)
{
#if _DEBUG
    CloseHandle(file);
#endif
}

//--------------------------------------------[ application_manager::application

const std::unordered_map<DWORD, bool>&
application_manager::application::processes() const
{
    return m_processes;
}

const std::wstring& application_manager::application::display_name() const
{
    return m_display_name;
}

bool application_manager::application::contains(DWORD pid) const
{
    return m_processes.find(pid) != m_processes.end();
}

//---------------------------------------------------------[ application_manager

template <typename T> static inline void safe_release(T** out_COM_obj)
{
    static_assert(std::is_base_of<IUnknown, T>::value,
        "Object must implement IUnknown");
    if (*out_COM_obj)
        (*out_COM_obj)->Release();
    *out_COM_obj = nullptr;
}

const std::unordered_map<std::string, application_manager::application>&
application_manager::applications() const
{
    return m_applications;
}

void application_manager::add(const std::string& session_name,
    const std::wstring& display_name, DWORD pid,
    bool x64)
{
    if (m_applications.find(session_name) == m_applications.end()) {
        m_applications[session_name] = {};
        m_applications[session_name].m_display_name = display_name;
    }
    m_applications[session_name].m_processes[pid] = x64;
}

void application_manager::clear()
{
    m_applications.clear();
}

bool application_manager::contains(std::string& session_name) const
{
    return m_applications.find(session_name) != m_applications.end();
}

size_t application_manager::size() const
{
    return m_applications.size();
}

bool application_manager::refresh()
{
    if (FAILED(CoInitialize(NULL)))
        return false;

    bool success = false;
    IMMDeviceEnumerator* device_enum = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager2* session_manager = nullptr;
    IAudioSessionEnumerator* session_enum = nullptr;
    IAudioSessionControl* session_control = nullptr;
    IAudioSessionControl2* session_control2 = nullptr;

    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&device_enum)))
        goto exit_uninitialize;

    if (FAILED(device_enum->GetDefaultAudioEndpoint(
            eRender, eMultimedia, &device)))
        goto exit_release_device_enum;

    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), 0,
            nullptr, (void**)&session_manager)))
        goto exit_release_device;

    if (FAILED(session_manager->GetSessionEnumerator(&session_enum)))
        goto exit_release_session_manager;

    int session_count;
    session_enum->GetCount(&session_count);
    clear();
    m_applications.reserve(session_count);

    for (int i = 0; i < session_count; i++) {
        if (FAILED(session_enum->GetSession(i, &session_control)))
            continue;

        if (FAILED(session_control->QueryInterface(
                &session_control2))) {
            safe_release(&session_control);
            continue;
        }

        DWORD pid;
        session_control2->GetProcessId(&pid);
        HANDLE h_process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);

        wchar_t* display_name;
        session_control->GetDisplayName(&display_name);

        char session_name[MAX_PATH];
        DWORD session_name_len = GetModuleBaseNameA(
            h_process, NULL, session_name, MAX_PATH);

        if (session_name_len > 0) {
            BOOL x32 = true;
#ifdef _WIN64
            IsWow64Process(h_process, &x32);
#endif
            add(session_name, display_name, pid, !x32);
        }

        CloseHandle(h_process);
        safe_release(&session_control2);
        safe_release(&session_control);
    }
    success = true;

    safe_release(&session_enum);
exit_release_session_manager:
    safe_release(&session_manager);
exit_release_device:
    safe_release(&device);
exit_release_device_enum:
    safe_release(&device_enum);
exit_uninitialize:
    CoUninitialize();
    return success;
}

audio_metadata audio_device_metadata()
{
    audio_metadata ret {
        .layout = speaker_layout::SPEAKERS_STEREO,
        .format = audio_format::AUDIO_FORMAT_FLOAT,
        .sample_rate = 44100,
    };

    if (FAILED(CoInitialize(NULL)))
        return ret;

    IMMDeviceEnumerator* device_enum = nullptr;
    IMMDevice* device = nullptr;
    IPropertyStore* store = nullptr;
    PROPVARIANT prop;

    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&device_enum)))
        goto exit_uninitialize;

    if (FAILED(device_enum->GetDefaultAudioEndpoint(
            eRender, eMultimedia, &device)))
        goto exit_release_device_enum;

    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)))
        goto exit_release_device;

    if (FAILED(store->GetValue(PKEY_AudioEngine_DeviceFormat, &prop)))
        goto exit_release_store;

    ret = wfmt_to_md((WAVEFORMATEX*)prop.blob.pBlobData);

exit_release_store:
    safe_release(&store);
exit_release_device:
    safe_release(&device);
exit_release_device_enum:
    safe_release(&device_enum);
exit_uninitialize:
    CoUninitialize();
    return ret;
}

audio_metadata wfmt_to_md(WAVEFORMATEX* wfmt)
{
    audio_metadata md;

    md.sample_rate = wfmt->nSamplesPerSec;

    if (wfmt->cbSize < 22) {
        if (wfmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            md.format = AUDIO_FORMAT_FLOAT;
        else if (wfmt->wBitsPerSample == 8)
            md.format = AUDIO_FORMAT_U8BIT;
        else if (wfmt->wBitsPerSample == 16)
            md.format = AUDIO_FORMAT_16BIT;
        else if (wfmt->wBitsPerSample == 32)
            md.format = AUDIO_FORMAT_32BIT;
        else
            md.format = AUDIO_FORMAT_UNKNOWN;

        md.layout = (speaker_layout)wfmt->nChannels;
    } else {
        auto &extensible = *(WAVEFORMATEXTENSIBLE*)&wfmt;

        GUID fmt = extensible.SubFormat;

        if (fmt == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            md.format = AUDIO_FORMAT_FLOAT;
        else if (fmt == KSDATAFORMAT_SUBTYPE_PCM) {
            if (wfmt->wBitsPerSample == 8)
                md.format = AUDIO_FORMAT_U8BIT;
            else if (wfmt->wBitsPerSample == 16)
                md.format = AUDIO_FORMAT_16BIT;
            else if (wfmt->wBitsPerSample == 32)
                md.format = AUDIO_FORMAT_32BIT;
            else
                md.format = AUDIO_FORMAT_UNKNOWN;
        } else {
            md.format = AUDIO_FORMAT_UNKNOWN;
        }

        DWORD layout = extensible.dwChannelMask;
        switch (layout) {
        case KSAUDIO_SPEAKER_2POINT1:
            md.layout = SPEAKERS_2POINT1;
            break;
        case KSAUDIO_SPEAKER_SURROUND:
            md.layout = SPEAKERS_4POINT0;
            break;
        case (KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY):
            md.layout = SPEAKERS_4POINT1;
            break;
        case KSAUDIO_SPEAKER_5POINT1_SURROUND:
            md.layout = SPEAKERS_5POINT1;
            break;
        case KSAUDIO_SPEAKER_7POINT1_SURROUND:
            md.layout = SPEAKERS_7POINT1;
            break;
        default:
            md.layout = (speaker_layout)wfmt->nChannels;
        }
    }

    return md;
}
