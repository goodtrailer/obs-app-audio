#include "audio-helpers.h"
#include "audio-hook-info.h"

#include <codecvt>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <Windows.h>

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

// clang-format off

// --------------------------------------------------------------------[ setting

#define SETTING_TARGET_PROCESS          "target_application"
#define SETTING_UPDATE_RATE             "update_rate"
#define SETTING_BUFFER                  "buffer"

// ----------------------------------------------------------------------[ label

#define LABEL_AUDIO_CAPTURE             obs_module_text("AppAudioCapture")

#define LABEL_TARGET_APPLICATION        obs_module_text("AppAudioCapture.TargetApplication")

#define LABEL_UPDATE_RATE               obs_module_text("AppAudioCapture.UpdateRate")
#define LABEL_UPDATE_RATE_SLOW          obs_module_text("AppAudioCapture.UpdateRate.Slow")
#define LABEL_UPDATE_RATE_NORMAL        obs_module_text("AppAudioCapture.UpdateRate.Normal")
#define LABEL_UPDATE_RATE_FAST          obs_module_text("AppAudioCapture.UpdateRate.Fast")
#define LABEL_UPDATE_RATE_FASTEST       obs_module_text("AppAudioCapture.UpdateRate.Fastest")

#define LABEL_BUFFER                    obs_module_text("AppAudioCapture.Buffer")
#define LABEL_BUFFER_SMALLEST           obs_module_text("AppAudioCapture.Buffer.Smallest")
#define LABEL_BUFFER_SMALL              obs_module_text("AppAudioCapture.Buffer.Small")
#define LABEL_BUFFER_NORMAL             obs_module_text("AppAudioCapture.Buffer.Normal")
#define LABEL_BUFFER_BIGGEST            obs_module_text("AppAudioCapture.Buffer.Biggest")

// --------------------------------------------------------------------[ tooltip

#define TOOLTIP_UPDATE_RATE             obs_module_text("AppAudioCapture.UpdateRate.Tooltip")
#define TOOLTIP_BUFFER                  obs_module_text("AppAudioCapture.Buffer.Tooltip")

// -----------------------------------------------------------------------[ misc

// update rate in nanoseconds
#define UPDATE_RATE_SLOW                2'000'000'000
#define UPDATE_RATE_NORMAL              1'000'000'000
#define UPDATE_RATE_FAST                500'000'000
#define UPDATE_RATE_FASTEST             250'000'000

// buffer duration in nanoseconds
#define BUFFER_SMALLEST                 240'000'000
#define BUFFER_SMALL                    360'000'000
#define BUFFER_NORMAL                   480'000'000
#define BUFFER_BIGGEST                  600'000'000

// clang-format on

using namespace std::placeholders;

struct app_audio_capture_data {
    obs_source_t* source = nullptr;

    uint32_t update_rate = 0;
    uint32_t buffer = 0;
    std::string target_session_name;

    bool initialized_thread = false;
    pthread_t thread = {};
    os_event_t* event = nullptr;

    const audio_metadata metadata;
    const win_pipe::receiver receiver;
    audio_mixer mixer;
    application_manager app_manager;

    app_audio_capture_data(size_t size = 0)
        : metadata { audio_device_metadata() }
        , receiver {
            AUDIO_PIPE_NAME,
            std::bind(&app_audio_capture_data::callback, this, _1, _2)
        }
        , mixer { size, metadata }
    {
    }

private:
    void callback(uint8_t* buffer, size_t size)
    {
        pipe_metadata* md = (pipe_metadata*)buffer;
        const uint8_t* data = (uint8_t*)buffer + sizeof(md);

        size_t data_size = size - sizeof(md);

        size_t index = mixer.calculate_index(md->timestamp);
        mixer.mix_frames((AUDIO_PIPE_ASSUMED_TYPE*)data, data_size, index);
    }
};

bool check_file_integrity(std::string filepath)
{
    HANDLE file = CreateFileA(filepath.c_str(), GENERIC_READ | GENERIC_EXECUTE,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return true;
    }

    blog(LOG_WARNING, "obs-app-audio file \"%s\" couldn't be loaded: %lu",
        filepath.c_str(), GetLastError());

    return false;
}

bool create_dll_injector_proc(std::string dll_injector_path,
    std::string dll_path, DWORD pid)
{
    PROCESS_INFORMATION pi = { 0 };
    STARTUPINFOA si = { 0 };
    si.cb = sizeof(si);

    char command_line[MAX_PATH * 3] = { 0 };
    snprintf(command_line, sizeof(command_line), "\"%s\" \"%s\" %lu",
        dll_injector_path.c_str(), dll_path.c_str(), pid);

    bool success = CreateProcessA(NULL, command_line, NULL, NULL, false,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    if (success) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        blog(LOG_WARNING, "Failed to create DLL injector process: %lu",
            GetLastError());
    }

    return success;
}

inline std::string path_to_absolute(std::string_view path)
{
    return std::filesystem::absolute(path).string();
}

void inject_hooks(const std::unordered_map<DWORD, bool>& processes)
{
    auto injector_path_32 = path_to_absolute(obs_module_file("dll-injector32.exe"));
    auto hook_path_32 = path_to_absolute(obs_module_file("audio-hook32.dll"));

    if (!check_file_integrity(injector_path_32) || !check_file_integrity(hook_path_32))
        return;

#ifdef _WIN64
    auto injector_path_64 = path_to_absolute(obs_module_file("dll-injector64.exe"));
    auto hook_path_64 = path_to_absolute(obs_module_file("audio-hook64.dll"));

    if (!check_file_integrity(injector_path_64) || !check_file_integrity(hook_path_64))
        return;
#endif

    for (const auto& [pid, x64] : processes) {
#ifdef _WIN64
        if (x64)
            create_dll_injector_proc(injector_path_64, hook_path_64, pid);
        else
#endif
            create_dll_injector_proc(injector_path_32, hook_path_32, pid);
    }
}

bool ensure_target_app_listed(obs_properties*, obs_property* list, obs_data* settings)
{
    std::string name = obs_data_get_string(settings, SETTING_TARGET_PROCESS);

    if (name.size() == 0)
        return false;

    bool match = false;
    size_t count = obs_property_list_item_count(list);
    for (size_t i = 0; i < count; i++) {
        std::string value = obs_property_list_item_string(list, i);
        if (name == value) {
            match = true;
            break;
        }
    }

    if (!match) {
        std::string entry = "[" + name + "] Not currently open.";
        obs_property_list_insert_string(list, 0, entry.c_str(), name.c_str());
        obs_property_list_item_disable(list, 0, true);
        return true;
    }

    return false;
}

bool fill_app_list(const application_manager& app_manager, obs_property* list)
{
    auto apps = app_manager.applications();
    for (auto& [name, app] : apps) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        auto display_name = converter.to_bytes(app.display_name());
        auto listing = "[" + name + "] " + display_name;

        obs_property_list_add_string(list, listing.c_str(), name.c_str());
    }
    return true;
}

void update_apps(app_audio_capture_data* aacd)
{
    aacd->app_manager.refresh();

    std::unordered_set<DWORD> pids;
    if (aacd->app_manager.contains(aacd->target_session_name)) {
        auto procs = aacd->app_manager.applications()
                         .at(aacd->target_session_name)
                         .processes();
        for (auto& [pid, x64] : procs)
            pids.insert(pid);
        inject_hooks(procs);
    }
}

void output_audio(app_audio_capture_data* aacd)
{
    uint64_t timestamp = aacd->mixer.timestamp();
    std::vector<AUDIO_PIPE_ASSUMED_TYPE> samples = aacd->mixer.pop();

    obs_source_audio audio;
    audio.data[0] = (uint8_t*)samples.data();
    audio.frames = (uint32_t)samples.size();
    audio.samples_per_sec = aacd->metadata.sample_rate;
    audio.format = aacd->metadata.format;
    audio.speakers = aacd->metadata.layout;
    audio.timestamp = timestamp;

    obs_source_output_audio(aacd->source, &audio);
}

void* audio_capture_thread(void* data)
{
    auto aacd = (app_audio_capture_data*)data;

    uint64_t last_update = os_gettime_ns();
    uint64_t now = 0;

    while (os_event_try(aacd->event) == EAGAIN) {
        now = os_gettime_ns();

        // update cycle (injecting dll)
        if (now - last_update < aacd->update_rate) {
            update_apps(aacd);
            last_update = os_gettime_ns();
        }

        // obs audio output cycle
        if (aacd->mixer.ready_to_pop())
            output_audio(aacd);
    }
    return NULL;
}

// -----------------------------------------------------[ app_audio_capture_info

const char* app_audio_capture_name(void*)
{
    return LABEL_AUDIO_CAPTURE;
}

void app_audio_capture_destroy(void* data)
{
    auto aacd = (app_audio_capture_data*)data;

    if (!aacd)
        return;

    if (aacd->initialized_thread) {
        os_event_signal(aacd->event);
        pthread_join(aacd->thread, NULL);
    }

    os_event_destroy(aacd->event);
    delete aacd;
}

void app_audio_capture_defaults(obs_data* settings)
{
    obs_data_set_default_int(settings, SETTING_UPDATE_RATE, UPDATE_RATE_NORMAL);
    obs_data_set_default_string(settings, SETTING_TARGET_PROCESS, "");
    obs_data_set_default_int(settings, SETTING_BUFFER, BUFFER_NORMAL);
}

void app_audio_capture_update(void* data, obs_data* settings)
{
    auto aacd = (app_audio_capture_data*)data;

    aacd->update_rate = (uint32_t)obs_data_get_int(settings, SETTING_UPDATE_RATE);
    aacd->buffer = (uint32_t)obs_data_get_int(settings, SETTING_BUFFER);
    aacd->target_session_name = obs_data_get_string(settings, SETTING_TARGET_PROCESS);

    size_t size = (size_t)aacd->buffer * aacd->metadata.sample_rate / 1'000'000'000;
    aacd->mixer.resize(size);
}

void* app_audio_capture_create(obs_data* settings, obs_source* source)
{
    app_audio_capture_data* aacd = new app_audio_capture_data;

    aacd->source = source;
    app_audio_capture_update(aacd, settings);

    if (os_event_init(&aacd->event, OS_EVENT_TYPE_MANUAL) != 0)
        goto fail;
    if (pthread_create(&aacd->thread, NULL, audio_capture_thread, aacd) != 0)
        goto fail;

    aacd->initialized_thread = true;

    return aacd;

fail:
    app_audio_capture_destroy(aacd);
    return NULL;
}

obs_properties* app_audio_capture_properties(void* data)
{
    auto* aacd = (app_audio_capture_data*)data;

    obs_properties* ppts = obs_properties_create();

    obs_property* app_list = obs_properties_add_list(
        ppts, SETTING_TARGET_PROCESS, LABEL_TARGET_APPLICATION,
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(app_list, "", "");
    fill_app_list(aacd->app_manager, app_list);
    obs_property_modified_t callback = ensure_target_app_listed;

    obs_property_set_modified_callback(app_list, callback);

    obs_property* update_rate_list = obs_properties_add_list(
        ppts, SETTING_UPDATE_RATE, LABEL_UPDATE_RATE, OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_SLOW, UPDATE_RATE_SLOW);
    obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_NORMAL, UPDATE_RATE_NORMAL);
    obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_FAST, UPDATE_RATE_FAST);
    obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_FASTEST, UPDATE_RATE_FASTEST);
    obs_property_set_long_description(update_rate_list, TOOLTIP_UPDATE_RATE);

    obs_property* buffer_list = obs_properties_add_list(
        ppts, SETTING_BUFFER, LABEL_BUFFER, OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(buffer_list, LABEL_BUFFER_SMALLEST, BUFFER_SMALLEST);
    obs_property_list_add_int(buffer_list, LABEL_BUFFER_SMALL, BUFFER_SMALL);
    obs_property_list_add_int(buffer_list, LABEL_BUFFER_NORMAL, BUFFER_NORMAL);
    obs_property_list_add_int(buffer_list, LABEL_BUFFER_BIGGEST, BUFFER_BIGGEST);
    obs_property_set_long_description(buffer_list, TOOLTIP_BUFFER);

    return ppts;
}

OBS_DECLARE_MODULE()

OBS_MODULE_USE_DEFAULT_LOCALE("obs-app-audio", "en-US")

MODULE_EXPORT const char* obs_module_description(void)
{
    return "Windows single-application audio capture";
}

bool obs_module_load(void)
{
    obs_source_info app_audio_capture_info {
        .id = "app_audio_capture",
        .type = OBS_SOURCE_TYPE_INPUT,
        .output_flags = OBS_SOURCE_AUDIO,
        .get_name = app_audio_capture_name,
        .create = app_audio_capture_create,
        .destroy = app_audio_capture_destroy,
        .get_defaults = app_audio_capture_defaults,
        .get_properties = app_audio_capture_properties,
        .update = app_audio_capture_update,
        .icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT
    };

    obs_register_source(&app_audio_capture_info);

    return true;
}
