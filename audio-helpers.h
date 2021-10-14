#pragma once
#include "audio-hook-info.h"
#include "win-pipe/win-pipe.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <media-io/audio-io.h>
#include <util/platform.h>

struct audio_metadata {
    speaker_layout layout;
    audio_format format;
    uint32_t sample_rate;
};

class audio_mixer {
public:
    audio_mixer(size_t size, audio_metadata metadata)
        : m_sample_rate { metadata.sample_rate }
        , m_channels { get_audio_channels(metadata.layout) }
    {
        resize(size);
    }

    size_t calculate_index(uint64_t timestamp) const
    {
        return (size_t)((timestamp - m_timestamp) * m_sample_rate * 1e-9);
    }

    void resize(size_t size)
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

    size_t size() const
    {
        return m_size;
    }

    uint64_t timestamp() const
    {
        return m_timestamp;
    }

    bool ready_to_pop() const
    {
        uint64_t delta = os_gettime_ns() - m_timestamp;
        return delta > calculate_duration(m_size / NUM_VECS);
    }

    std::vector<AUDIO_PIPE_ASSUMED_TYPE> pop()
    {
        std::lock_guard lock = std::lock_guard(m_mutex);

        std::vector<AUDIO_PIPE_ASSUMED_TYPE> ret = m_vecs[0];
        m_vecs.pop_front();
        m_vecs.emplace_back(ret.size());

        m_timestamp += calculate_duration(m_size / NUM_VECS);
        return ret;
    }

    void mix_frames(const AUDIO_PIPE_ASSUMED_TYPE* samples_buffer, size_t size,
        size_t index)
    {
        std::lock_guard lock = std::lock_guard(m_mutex);

        size_t vec_size = m_size / NUM_VECS;

        unsigned int vec = (unsigned int)(index / vec_size);
        unsigned int vec_index = (unsigned int)(index % vec_size);

        size_t sample = 0;
        size_t samples_count = size / sizeof(AUDIO_PIPE_ASSUMED_TYPE);

        while (sample < samples_count && vec < m_vecs.size()) {
            m_vecs[vec][vec_index] += samples_buffer[sample];

            sample++;
            vec_index++;
            if (vec_index >= vec_size) {
                vec_index = 0;
                vec++;
            }
        }
    }

    void mix_frames(const std::vector<AUDIO_PIPE_ASSUMED_TYPE>& frames,
        size_t index)
    {
        mix_frames(frames.data(), frames.size(), index);
    }

public:
    static constexpr int NUM_VECS = 3;

private:
    uint64_t calculate_duration(size_t size) const
    {
        return (uint64_t)(size * 1e9) / m_sample_rate;
    }

private:
    std::deque<std::vector<AUDIO_PIPE_ASSUMED_TYPE>> m_vecs;

    uint64_t m_timestamp;
    uint32_t m_sample_rate;
    uint32_t m_channels;

    size_t m_size;
    std::mutex m_mutex;
};

class application_manager {
public:
    class application {
        friend class application_manager;

    public:
        const std::unordered_map<DWORD, bool>& processes() const;
        const std::wstring& display_name() const;
        bool contains(DWORD pid) const;

    private:
        std::unordered_map<DWORD, bool> m_processes;
        std::wstring m_display_name;
    };

public:
    const std::unordered_map<std::string, application>& applications() const;
    void add(const std::string& session_name, const std::wstring& display_name,
        DWORD pid, bool x64);
    void clear();
    bool contains(std::string& session_name) const;
    size_t size() const;
    bool refresh();

private:
    std::unordered_map<std::string, application> m_applications;
};

WAVEFORMATEX audio_device_format();

audio_metadata wfmt_to_md(WAVEFORMATEX wfmt);
