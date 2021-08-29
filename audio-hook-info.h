#pragma once
#include <media-io/audio-io.h>
#include <stdint.h>

// clang-format off

#define AUDIO_PIPE_NAME                 "AudioHook_Pipe"

#define AUDIO_RESAMPLE_CHANNELS         2
#define AUDIO_RESAMPLE_SAMPLE_SIZE      sizeof(float)
#define AUDIO_RESAMPLE_FRAME_SIZE       (AUDIO_RESAMPLE_CHANNELS * AUDIO_RESAMPLE_SAMPLE_SIZE)

#define AUDIO_RESAMPLE_SPEAKERS         SPEAKERS_STEREO
#define AUDIO_RESAMPLE_AUDIO_FORMAT     AUDIO_FORMAT_FLOAT
#define AUDIO_RESAMPLE_SAMPLE_RATE      44100

#define AUDIO_RESAMPLE_AV_CH_LAYOUT     AV_CH_LAYOUT_STEREO
#define AUDIO_RESAMPLE_AV_SAMPLE_FMT    AV_SAMPLE_FMT_FLT


// clang-format on

struct audio_metadata {
    speaker_layout layout;
    audio_format format;
    int samples_per_sec;
    uint32_t frames;
};
