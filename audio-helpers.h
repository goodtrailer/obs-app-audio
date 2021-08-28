#pragma once
#include "audio-hook-info.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ipc-util/pipe.h>
#include <media-io/audio-io.h>
#pragma warning(disable : 4244)
extern "C" {
#include <libswresample/swresample.h>
}
#pragma warning(default : 4244)

struct audio_frame {
	float samples[AUDIO_RESAMPLE_CHANNELS];
};

int64_t obs_layout_to_swr_layout(enum speaker_layout layout);

AVSampleFormat obs_format_to_swr_format(audio_format format);

// Uses 1/NUM_VECS of its size to buffer the past, just in case of shenanigans.
// The rest of its size, which is (NUM_VECS - 1)/NUM_VECS, is for buffering
// the future.
class audio_mixer {
public:
	audio_mixer(size_t size = 0);

	size_t calculate_index(uint64_t timestamp) const;

	static size_t calculate_size(uint64_t duration);

	uint64_t calculate_timestamp(size_t index) const;

	static uint64_t calculate_duration(size_t size);

	void resize(size_t size);

	size_t size() const;

	bool ready_to_pop() const;

	uint64_t timestamp() const;

	std::vector<audio_frame> pop();

	void mix_frames(const audio_frame *frames_buffer, size_t frames_count,
			size_t index);

	void mix_frames(const std::vector<audio_frame> &frames, size_t index);

public:
	static constexpr int NUM_VECS = 3;

private:
	std::deque<std::vector<audio_frame>> m_vecs;

	uint64_t m_timestamp;

	size_t m_size;

	std::mutex m_mutex;
};

class audio_pipe_manager {
private:
	class audio_pipe {
		friend class audio_pipe_manager;

	public:
		audio_pipe() = default;

		audio_pipe(const std::string &name, DWORD target_pid, audio_mixer *mixer);

		audio_pipe(audio_pipe &&) = delete;

		~audio_pipe();

		void read(uint8_t *buffer, size_t size);

		static void pipe_read(void *self, uint8_t *buffer, size_t size);

	private:
		bool m_initialized = false;

		ipc_pipe_server_t m_pipe_server;

		audio_mixer *m_mixer = nullptr;

		SwrContext *m_swr_ctx = nullptr;

		DWORD m_target_pid = 0;

		int64_t m_layout = 0;

		AVSampleFormat m_format = AV_SAMPLE_FMT_NONE;

		int m_sample_rate = 0;

		uint64_t m_last_timestamp = 0;
	};

public:
	audio_pipe_manager() = default;

	audio_pipe_manager(audio_mixer &mixer);

	void set_mixer(audio_mixer &mixer);

	bool add(DWORD pid);

	void remove(DWORD pid);

	void clear();

	bool contains(DWORD pid) const;

	size_t size() const;

	void target(const std::unordered_set<DWORD> &pids);

private:
	std::unordered_map<DWORD, audio_pipe> m_pipes;

	audio_mixer *m_mixer = nullptr;
};

class application_manager {
public:
	class application {
		friend class application_manager;

	public:
		const std::unordered_map<DWORD, bool> &processes() const;

		const std::wstring &display_name() const;

		bool contains(DWORD pid) const;

	private:
		std::unordered_map<DWORD, bool> m_processes;

		std::wstring m_display_name;
	};

public:
	application_manager() = default;

	const std::unordered_map<std::string, application> &
	applications() const;

	void add(const std::string &session_name,
		 const std::wstring &display_name, DWORD pid, bool x64);

	void clear();

	bool contains(std::string &session_name) const;

	size_t size() const;

	bool refresh();

private:
	std::unordered_map<std::string, application> m_applications;
};
