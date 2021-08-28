#include "audio-helpers.h"
#include "audio-hook-info.h"

#include <codecvt>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <Windows.h>

#include <obs-module.h>
#include <ipc-util/pipe.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#pragma warning(disable : 4244)
#include <libswresample/swresample.h>
#pragma warning(default : 4244)

/*---------[SETTINGS]---------*/

#define SETTING_TARGET_PROCESS "target_application"
#define SETTING_UPDATE_RATE "update_rate"
#define SETTING_BUFFER "buffer"

/*----------[LABELS]----------*/

#define LABEL_AUDIO_CAPTURE obs_module_text("AppAudioCapture")

#define LABEL_TARGET_APPLICATION \
	obs_module_text("AppAudioCapture.TargetApplication")

#define LABEL_UPDATE_RATE obs_module_text("AppAudioCapture.UpdateRate")
#define LABEL_UPDATE_RATE_SLOW \
	obs_module_text("AppAudioCapture.UpdateRate.Slow")
#define LABEL_UPDATE_RATE_NORMAL \
	obs_module_text("AppAudioCapture.UpdateRate.Normal")
#define LABEL_UPDATE_RATE_FAST \
	obs_module_text("AppAudioCapture.UpdateRate.Fast")
#define LABEL_UPDATE_RATE_FASTEST \
	obs_module_text("AppAudioCapture.UpdateRate.Fastest")

#define LABEL_BUFFER obs_module_text("AppAudioCapture.Buffer")
#define LABEL_BUFFER_SMALLEST obs_module_text("AppAudioCapture.Buffer.Smallest")
#define LABEL_BUFFER_SMALL obs_module_text("AppAudioCapture.Buffer.Small")
#define LABEL_BUFFER_NORMAL obs_module_text("AppAudioCapture.Buffer.Normal")
#define LABEL_BUFFER_BIGGEST obs_module_text("AppAudioCapture.Buffer.Biggest")

/*---------[TOOLTIPS]---------*/

#define TOOLTIP_UPDATE_RATE \
	obs_module_text("AppAudioCapture.UpdateRate.Tooltip")
#define TOOLTIP_BUFFER obs_module_text("AppAudioCapture.Buffer.Tooltip")

/*-----------[MISC]-----------*/

#define do_log(level, format, ...) blog(level, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

// update rate in nanoseconds
#define UPDATE_RATE_SLOW 2'000'000'000
#define UPDATE_RATE_NORMAL 1'000'000'000
#define UPDATE_RATE_FAST 500'000'000
#define UPDATE_RATE_FASTEST 250'000'000

// buffer duration in nanoseconds
#define BUFFER_SMALLEST 240'000'000
#define BUFFER_SMALL 360'000'000
#define BUFFER_NORMAL 480'000'000
#define BUFFER_BIGGEST 600'000'000

struct app_audio_capture_data {
	obs_source_t *source = nullptr;

	uint32_t update_rate = 0;
	uint32_t buffer = 0;
	std::string target_session_name;

	bool initialized_thread = false;
	pthread_t thread = {};
	os_event_t *event = nullptr;

	application_manager app_manager;
	audio_pipe_manager pipe_manager;
	audio_mixer mixer;

	app_audio_capture_data() { pipe_manager.set_mixer(mixer); }
};

bool check_file_integrity(std::string filepath)
{
	HANDLE file =
		CreateFileA(filepath.c_str(), GENERIC_READ | GENERIC_EXECUTE,
			    FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
		return true;
	}

	DWORD error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND)
		warn("Audio capture file '%s' not found.", filepath.c_str());
	else if (error == ERROR_ACCESS_DENIED)
		warn("Audio capture file '%s' could not be loaded.",
		     filepath.c_str());
	else
		warn("Audio capture file '%s' could not be loaded: %lu.",
		     filepath.c_str(), error);

	return false;
}

bool create_dll_injector_proc(std::string dll_injector_path,
			      std::string dll_path, DWORD pid)
{
	PROCESS_INFORMATION pi = {0};
	STARTUPINFOA si = {0};
	si.cb = sizeof(si);

	char command_line[MAX_PATH * 3] = {0};
	snprintf(command_line, sizeof(command_line), "\"%s\" \"%s\" %lu",
		 dll_injector_path.c_str(), dll_path.c_str(), pid);

	bool success = CreateProcessA(NULL, command_line, NULL, NULL, false,
				      CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

	if (success) {
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	} else
		warn("Failed to create DLL injector process: %lu",
		     GetLastError());

	return success;
}

void inject_hooks(app_audio_capture_data *aacd)
{
	if (!aacd->app_manager.contains(aacd->target_session_name))
		return;

	std::string injector_path_32 =
		std::filesystem::absolute(obs_module_file("dll-injector32.exe"))
			.string();
	std::string hook_path_32 =
		std::filesystem::absolute(obs_module_file("audio-hook32.dll"))
			.string();

	if (!check_file_integrity(injector_path_32) ||
	    !check_file_integrity(hook_path_32))
		return;

#ifdef _WIN64
	std::string injector_path_64 =
		std::filesystem::absolute(obs_module_file("dll-injector64.exe"))
			.string();
	std::string hook_path_64 =
		std::filesystem::absolute(obs_module_file("audio-hook64.dll"))
			.string();

	if (!check_file_integrity(injector_path_64) ||
	    !check_file_integrity(hook_path_64))
		return;
#endif

	auto &application =
		aacd->app_manager.applications().at(aacd->target_session_name);

	for (const auto &[pid, x64] : application.processes()) {
#ifdef _WIN64
		if (x64)
			create_dll_injector_proc(injector_path_64, hook_path_64,
						 pid);
		else
#endif
			create_dll_injector_proc(injector_path_32, hook_path_32,
						 pid);
	}
}

bool ensure_target_app_listed(obs_property_t *list, obs_data_t *settings,
			      const char *setting_name, size_t index = 0)
{
	std::string target = obs_data_get_string(settings, setting_name);

	if (target.size() == 0)
		return false;

	bool match = false;
	size_t count = obs_property_list_item_count(list);
	for (size_t i = 0; i < count; i++) {
		std::string value = obs_property_list_item_string(list, i);
		if (target == value) {
			match = true;
			break;
		}
	}

	if (!match) {
		std::ostringstream listing;
		listing << "[" << target << "] Not currently open.";
		obs_property_list_insert_string(
			list, index, listing.str().c_str(), target.c_str());
		obs_property_list_item_disable(list, index, true);
		return true;
	}

	return false;
}

bool fill_app_list(app_audio_capture_data *aacd, obs_property_t *list)
{
	auto &apps = aacd->app_manager.applications();
	for (auto &[name, app] : apps) {
		std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
		auto display_name = converter.to_bytes(app.display_name());
		auto listing = "[" + name + "] " + display_name;

		obs_property_list_add_string(list, listing.c_str(),
					     name.c_str());
	}
	return true;
}

void update_apps_and_pipes(app_audio_capture_data *aacd)
{
	aacd->app_manager.refresh();

	std::unordered_set<DWORD> pids;
	if (aacd->app_manager.contains(aacd->target_session_name)) {
		auto &app = aacd->app_manager.applications().at(
			aacd->target_session_name);
		for (auto &[pid, x64] : app.processes())
			pids.insert(pid);
	}
	aacd->pipe_manager.target(pids);
	inject_hooks(aacd);
}

void output_audio(app_audio_capture_data *aacd)
{
	uint64_t timestamp = aacd->mixer.timestamp();
	std::vector<audio_frame> frames = aacd->mixer.pop();

	obs_source_audio audio;
	audio.data[0] = (uint8_t *)frames.data();
	audio.frames = (uint32_t)frames.size();
	audio.samples_per_sec = AUDIO_RESAMPLE_SAMPLE_RATE;
	audio.format = AUDIO_RESAMPLE_AUDIO_FORMAT;
	audio.speakers = AUDIO_RESAMPLE_SPEAKERS;
	audio.timestamp = timestamp;

	obs_source_output_audio(aacd->source, &audio);
}

void *audio_capture_thread(void *data)
{
	auto *aacd = (app_audio_capture_data *)data;

	uint64_t last_update = os_gettime_ns();
	uint64_t now = 0;

	while (os_event_try(aacd->event) == EAGAIN) {
		now = os_gettime_ns();

		// update cycle (injecting dll and refreshing pipes)
		if (now - last_update < aacd->update_rate) {
			update_apps_and_pipes(aacd);
			last_update = os_gettime_ns();
		}

		// obs audio output cycle
		if (aacd->mixer.ready_to_pop())
			output_audio(aacd);
	}
	return NULL;
}

/*----[AUDIO CAPTURE INFO]----*/

const char *app_audio_capture_name(void *)
{
	return LABEL_AUDIO_CAPTURE;
}

void app_audio_capture_destroy(void *data)
{
	auto *aacd = (app_audio_capture_data *)data;
	if (!aacd)
		return;

	if (aacd->initialized_thread) {
		os_event_signal(aacd->event);
		pthread_join(aacd->thread, NULL);
	}

	os_event_destroy(aacd->event);
	delete aacd;
}

void app_audio_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_UPDATE_RATE,
				 UPDATE_RATE_NORMAL);
	obs_data_set_default_string(settings, SETTING_TARGET_PROCESS, "");
	obs_data_set_default_int(settings, SETTING_BUFFER, BUFFER_NORMAL);
}

void app_audio_capture_update(void *data, obs_data_t *settings)
{
	auto *aacd = (app_audio_capture_data *)data;

	aacd->update_rate =
		(uint32_t)obs_data_get_int(settings, SETTING_UPDATE_RATE);
	aacd->buffer = (uint32_t)obs_data_get_int(settings, SETTING_BUFFER);
	aacd->target_session_name =
		obs_data_get_string(settings, SETTING_TARGET_PROCESS);

	aacd->mixer.resize(audio_mixer::calculate_size(aacd->buffer));
}

void *app_audio_capture_create(obs_data_t *settings, obs_source_t *source)
{
	app_audio_capture_data *aacd = new app_audio_capture_data;

	aacd->source = source;
	app_audio_capture_update(aacd, settings);

	if (os_event_init(&aacd->event, OS_EVENT_TYPE_MANUAL) != 0)
		goto fail;
	if (pthread_create(&aacd->thread, NULL, audio_capture_thread, aacd) !=
	    0)
		goto fail;

	aacd->initialized_thread = true;

	return aacd;

fail:
	app_audio_capture_destroy(aacd);
	return NULL;
}

obs_properties_t *app_audio_capture_properties(void *data)
{
	auto *aacd = (app_audio_capture_data *)data;
	obs_properties_t *ppts = obs_properties_create();

	obs_property_t *app_list = obs_properties_add_list(
		ppts, SETTING_TARGET_PROCESS, LABEL_TARGET_APPLICATION,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(app_list, "", "");
	fill_app_list(aacd, app_list);
	obs_property_modified_t callback = [](obs_properties_t *,
					      obs_property_t *list,
					      obs_data_t *settings) {
		return ensure_target_app_listed(list, settings,
						SETTING_TARGET_PROCESS);
	};
	obs_property_set_modified_callback(app_list, callback);

	obs_property_t *update_rate_list = obs_properties_add_list(
		ppts, SETTING_UPDATE_RATE, LABEL_UPDATE_RATE,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_SLOW,
				  UPDATE_RATE_SLOW);
	obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_NORMAL,
				  UPDATE_RATE_NORMAL);
	obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_FAST,
				  UPDATE_RATE_FAST);
	obs_property_list_add_int(update_rate_list, LABEL_UPDATE_RATE_FASTEST,
				  UPDATE_RATE_FASTEST);
	obs_property_set_long_description(update_rate_list,
					  TOOLTIP_UPDATE_RATE);

	obs_property_t *buffer_list = obs_properties_add_list(
		ppts, SETTING_BUFFER, LABEL_BUFFER, OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(buffer_list, LABEL_BUFFER_SMALLEST,
				  BUFFER_SMALLEST);
	obs_property_list_add_int(buffer_list, LABEL_BUFFER_SMALL,
				  BUFFER_SMALL);
	obs_property_list_add_int(buffer_list, LABEL_BUFFER_NORMAL,
				  BUFFER_NORMAL);
	obs_property_list_add_int(buffer_list, LABEL_BUFFER_BIGGEST,
				  BUFFER_BIGGEST);
	obs_property_set_long_description(buffer_list, TOOLTIP_BUFFER);

	return ppts;
}

OBS_DECLARE_MODULE()

OBS_MODULE_USE_DEFAULT_LOCALE("obs-app-audio", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Windows single-application audio capture";
}

bool obs_module_load(void)
{
	obs_source_info app_audio_capture_info{
		.id = "app_audio_capture",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_AUDIO,
		.get_name = app_audio_capture_name,
		.create = app_audio_capture_create,
		.destroy = app_audio_capture_destroy,
		.get_defaults = app_audio_capture_defaults,
		.get_properties = app_audio_capture_properties,
		.update = app_audio_capture_update,
		.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT};

	obs_register_source(&app_audio_capture_info);

	return true;
}
