#include "audio-helpers.h"

#include <functional>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmeapi.h>
#include <Psapi.h>
#include <Windows.h>

#include <obs-module.h>
#include <util/platform.h>

using namespace std::placeholders;

//----------------------------------------------------------[ obs/swr conversion

int64_t obs_layout_to_swr_layout(speaker_layout layout)
{
	switch (layout) {
	case SPEAKERS_MONO:
		return AV_CH_LAYOUT_MONO;
	case SPEAKERS_STEREO:
		return AV_CH_LAYOUT_STEREO;
	case SPEAKERS_2POINT1:
		return AV_CH_LAYOUT_2POINT1;
	case SPEAKERS_4POINT0:
		return AV_CH_LAYOUT_4POINT0;
	case SPEAKERS_4POINT1:
		return AV_CH_LAYOUT_4POINT1;
	case SPEAKERS_5POINT1:
		return AV_CH_LAYOUT_5POINT1;
	case SPEAKERS_7POINT1:
		return AV_CH_LAYOUT_7POINT1;
	default:
		return AV_CH_LAYOUT_STEREO;
	}
}

AVSampleFormat obs_format_to_swr_format(audio_format format)
{
	switch (format) {
	case AUDIO_FORMAT_FLOAT:
		return AV_SAMPLE_FMT_FLT;
	case AUDIO_FORMAT_16BIT:
		return AV_SAMPLE_FMT_S16;
	case AUDIO_FORMAT_32BIT:
		return AV_SAMPLE_FMT_S32;
	case AUDIO_FORMAT_U8BIT:
		return AV_SAMPLE_FMT_U8;
	case AUDIO_FORMAT_FLOAT_PLANAR:
		return AV_SAMPLE_FMT_FLTP;
	case AUDIO_FORMAT_16BIT_PLANAR:
		return AV_SAMPLE_FMT_S16P;
	case AUDIO_FORMAT_32BIT_PLANAR:
		return AV_SAMPLE_FMT_S32P;
	case AUDIO_FORMAT_U8BIT_PLANAR:
		return AV_SAMPLE_FMT_U8P;
	default:
		return AV_SAMPLE_FMT_FLT;
	}
}

//-----------------------------------------------------------------[ audio_mixer

audio_mixer::audio_mixer(size_t size)
{
	resize(size);
}

size_t audio_mixer::calculate_index(uint64_t timestamp) const
{
	return calculate_size(timestamp - m_timestamp);
}

size_t audio_mixer::calculate_size(uint64_t duration)
{
	return (size_t)(duration * AUDIO_RESAMPLE_SAMPLE_RATE * 1e-9);
}

uint64_t audio_mixer::calculate_timestamp(size_t index) const
{
	return calculate_duration(index) + m_timestamp;
}

uint64_t audio_mixer::calculate_duration(size_t size)
{
	return (uint64_t)(size * 1e9) / AUDIO_RESAMPLE_SAMPLE_RATE;
}

void audio_mixer::resize(size_t size)
{
	std::lock_guard lock = std::lock_guard(m_mutex);

	size_t individual_vec_size = size / NUM_VECS;
	if (individual_vec_size * NUM_VECS == m_size)
		return;

	m_size = individual_vec_size * NUM_VECS;

	m_vecs.clear();
	for (int i = 0; i < NUM_VECS; i++)
		m_vecs.emplace_back(individual_vec_size);

	m_timestamp = os_gettime_ns() - calculate_duration(m_size / NUM_VECS);
}

size_t audio_mixer::size() const
{
	return m_size;
}

bool audio_mixer::ready_to_pop() const
{
	uint64_t delta = os_gettime_ns() - m_timestamp;
	return delta > calculate_duration(m_size / NUM_VECS);
}

uint64_t audio_mixer::timestamp() const
{
	return m_timestamp;
}

std::vector<audio_frame> audio_mixer::pop()
{
	std::lock_guard lock = std::lock_guard(m_mutex);

	std::vector<audio_frame> ret = m_vecs[0];
	m_vecs.pop_front();
	m_vecs.emplace_back(ret.size());

	m_timestamp += calculate_duration(m_size / NUM_VECS);
	return ret;
}

void audio_mixer::mix_frames(const audio_frame *frames_buffer,
			     size_t frames_count, size_t index)
{
	std::lock_guard lock = std::lock_guard(m_mutex);

	size_t vec_size = m_size / NUM_VECS;

	unsigned int vec = (unsigned int)(index / vec_size);
	unsigned int vec_index = (unsigned int)(index % vec_size);
	size_t frame = 0;
	while (frame < frames_count && vec < m_vecs.size()) {
		for (int i = 0; i < AUDIO_RESAMPLE_CHANNELS; i++)
			m_vecs[vec][vec_index].samples[i] +=
				frames_buffer[frame].samples[i];

		frame++;
		vec_index++;
		if (vec_index >= vec_size) {
			vec_index = 0;
			vec++;
		}
	}
}

void audio_mixer::mix_frames(const std::vector<audio_frame> &frames,
			     size_t index)
{
	mix_frames(frames.data(), frames.size(), index);
}

//--------------------------------------------------------------------[ file out

HANDLE create_file(const char *path)
{
#if _DEBUG
	return CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
			   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
#endif
}

void write_file(HANDLE file, void *buffer, DWORD size)
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

//----------------------------------------------[ audio_pipe_manager::audio_pipe

audio_pipe_manager::audio_pipe::audio_pipe(std::string_view name,
					   audio_mixer *mixer)
	: m_receiver{name, std::bind(&audio_pipe::read, this, _1, _2)},
	  m_info{
		  .mixer = mixer,
		  .layout = AUDIO_RESAMPLE_AV_CH_LAYOUT,
		  .format = AUDIO_RESAMPLE_AV_SAMPLE_FMT,
		  .sample_rate = AUDIO_RESAMPLE_SAMPLE_RATE,
	  }
{
	// by default should basically do nothing and assume that the input
	// audio is the same encoding as the desired output
	m_info.swr_ctx = swr_alloc_set_opts(
		NULL, AUDIO_RESAMPLE_AV_CH_LAYOUT, AUDIO_RESAMPLE_AV_SAMPLE_FMT,
		AUDIO_RESAMPLE_SAMPLE_RATE, AUDIO_RESAMPLE_AV_CH_LAYOUT,
		AUDIO_RESAMPLE_AV_SAMPLE_FMT, AUDIO_RESAMPLE_SAMPLE_RATE, 0,
		NULL);

	swr_init(m_info.swr_ctx);
}

audio_pipe_manager::audio_pipe::audio_pipe(audio_pipe &&other) noexcept
	: m_receiver{std::move(other.m_receiver)}, m_info{other.m_info}
{
	other.m_info = {};

	m_receiver.set_callback(std::bind(&audio_pipe::read, this, _1, _2));
}

audio_pipe_manager::audio_pipe::~audio_pipe()
{
	if (m_info.swr_ctx)
		swr_free(&m_info.swr_ctx);
}

audio_pipe_manager::audio_pipe &
audio_pipe_manager::audio_pipe::operator=(audio_pipe &&other) noexcept
{
	m_receiver = std::move(other.m_receiver);
	m_info = other.m_info;
	other.m_info = {};

	m_receiver.set_callback(std::bind(&audio_pipe::read, this, _1, _2));

	return *this;
}

void audio_pipe_manager::audio_pipe::read(uint8_t *buffer, size_t size)
{
	auto *&mixer = m_info.mixer;
	auto *&swr_ctx = m_info.swr_ctx;
	auto &layout = m_info.layout;
	auto &format = m_info.format;
	auto &sample_rate = m_info.sample_rate;
	auto &last_timestamp = m_info.last_timestamp;

	if (size < sizeof(struct audio_metadata))
		return;

	uint64_t timestamp = os_gettime_ns();

	struct audio_metadata *md = (struct audio_metadata *)buffer;
	const uint8_t *data = (uint8_t *)buffer + sizeof(struct audio_metadata);

	int64_t av_layout = obs_layout_to_swr_layout(md->layout);
	enum AVSampleFormat av_format = obs_format_to_swr_format(md->format);

	if (av_layout != layout || av_format != format ||
	    md->samples_per_sec != sample_rate) {
		swr_ctx = swr_alloc_set_opts(NULL, AUDIO_RESAMPLE_AV_CH_LAYOUT,
					     AUDIO_RESAMPLE_AV_SAMPLE_FMT,
					     AUDIO_RESAMPLE_SAMPLE_RATE,
					     av_layout, av_format,
					     md->samples_per_sec, 0, NULL);
		swr_init(swr_ctx);
		layout = av_layout;
		format = av_format;
		sample_rate = md->samples_per_sec;
	}

	uint8_t *resampled_data;
	int resampled_frames =
		(int)av_rescale_rnd(md->frames, AUDIO_RESAMPLE_SAMPLE_RATE,
				    md->samples_per_sec, AV_ROUND_UP);
	av_samples_alloc(&resampled_data, NULL, AUDIO_RESAMPLE_CHANNELS,
			 resampled_frames, AUDIO_RESAMPLE_AV_SAMPLE_FMT, 0);
	resampled_frames = swr_convert(swr_ctx, &resampled_data,
				       resampled_frames, &data, md->frames);

	uint64_t expected_timestamp =
		last_timestamp + mixer->calculate_duration(resampled_frames);

	uint64_t deviation = timestamp < expected_timestamp
				     ? expected_timestamp - timestamp
				     : timestamp - expected_timestamp;
	uint64_t epsilon = audio_mixer::calculate_duration(mixer->size()) *
			   (audio_mixer::NUM_VECS - 1) / audio_mixer::NUM_VECS;

	// the timestamp is within expected random deviation
	if (deviation < epsilon)
		timestamp = expected_timestamp;

	size_t index = mixer->calculate_index(timestamp);
	mixer->mix_frames((struct audio_frame *)resampled_data,
			  resampled_frames, index);

	last_timestamp = timestamp;
	av_freep(&resampled_data);
}

//----------------------------------------------------------[ audio_pipe_manager

audio_pipe_manager::audio_pipe_manager(audio_mixer &mixer) : m_mixer(&mixer) {}

void audio_pipe_manager::set_mixer(audio_mixer &mixer)
{
	m_mixer = &mixer;
}

bool audio_pipe_manager::add(DWORD pid)
{
	if (contains(pid))
		return false;

	std::string name = AUDIO_PIPE_NAME + std::to_string(pid);
	m_pipes[pid] = audio_pipe{name, m_mixer};

	return true;
}

void audio_pipe_manager::remove(DWORD pid)
{
	m_pipes.erase(pid);
}

void audio_pipe_manager::clear()
{
	for (auto &[pid, pipe] : m_pipes)
		remove(pid);
}

bool audio_pipe_manager::contains(DWORD pid) const
{
	return m_pipes.find(pid) != m_pipes.end();
}

size_t audio_pipe_manager::size() const
{
	return m_pipes.size();
}

void audio_pipe_manager::target(const std::unordered_set<DWORD> &pids)
{
	for (auto &pid : pids)
		add(pid);

	for (auto it = m_pipes.begin(); it != m_pipes.end();) {
		const DWORD pid = it->first;
		it++;
		if (pids.find(pid) == pids.end())
			remove(pid);
	}
}

//--------------------------------------------[ application_manager::application

const std::unordered_map<DWORD, bool> &
application_manager::application::processes() const
{
	return m_processes;
}

const std::wstring &application_manager::application::display_name() const
{
	return m_display_name;
}

bool application_manager::application::contains(DWORD pid) const
{
	return m_processes.find(pid) != m_processes.end();
}

//---------------------------------------------------------[ application_manager

template<typename T> static inline void safe_release(T **out_COM_obj)
{
	static_assert(std::is_base_of<IUnknown, T>::value,
		      "Object must implement IUnknown");
	if (*out_COM_obj)
		(*out_COM_obj)->Release();
	*out_COM_obj = nullptr;
}

const std::unordered_map<std::string, application_manager::application> &
application_manager::applications() const
{
	return m_applications;
}

void application_manager::add(const std::string &session_name,
			      const std::wstring &display_name, DWORD pid,
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

bool application_manager::contains(std::string &session_name) const
{
	return m_applications.find(session_name) != m_applications.end();
}

size_t application_manager::size() const
{
	return m_applications.size();
}

bool application_manager::refresh()
{
	if (!SUCCEEDED(CoInitialize(NULL)))
		return false;

	bool success = false;
	IMMDeviceEnumerator *device_enum = nullptr;
	IMMDevice *device = nullptr;
	IAudioSessionManager2 *session_manager = nullptr;
	IAudioSessionEnumerator *session_enum = nullptr;
	IAudioSessionControl *session_control = nullptr;
	IAudioSessionControl2 *session_control2 = nullptr;

	if (!SUCCEEDED(CoCreateInstance(
		    __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		    __uuidof(IMMDeviceEnumerator), (void **)&device_enum)))
		goto out_uninitialize;

	if (!SUCCEEDED(device_enum->GetDefaultAudioEndpoint(
		    eRender, eMultimedia, &device)))
		goto out_release_device_enum;

	if (!SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), 0,
					nullptr, (void **)&session_manager)))
		goto out_release_device;

	if (!SUCCEEDED(session_manager->GetSessionEnumerator(&session_enum)))
		goto out_release_session_manager;

	int session_count;
	session_enum->GetCount(&session_count);
	clear();
	m_applications.reserve(session_count);

	for (int i = 0; i < session_count; i++) {
		if (!SUCCEEDED(session_enum->GetSession(i, &session_control)))
			continue;

		if (!SUCCEEDED(session_control->QueryInterface(
			    &session_control2))) {
			safe_release(&session_control);
			continue;
		}

		DWORD pid;
		session_control2->GetProcessId(&pid);
		HANDLE h_process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);

		wchar_t *display_name;
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
out_release_session_manager:
	safe_release(&session_manager);
out_release_device:
	safe_release(&device);
out_release_device_enum:
	safe_release(&device_enum);
out_uninitialize:
	CoUninitialize();
	return success;
}
