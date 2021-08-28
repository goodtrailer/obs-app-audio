#pragma once
#include <stdint.h>
#include <media-io/audio-io.h>

#define AUDIO_PIPE_NAME "AudioHook_Pipe"
#define AUDIO_PIPE_MAX_RETRY 4
#define SAFE_BUF_SIZE 12288
#define SAFE_DATA_SIZE (SAFE_BUF_SIZE - sizeof(audio_metadata))
#define MAX_BUF_COUNT 12

#define AUDIO_RESAMPLE_CHANNELS 2
#define AUDIO_RESAMPLE_AV_CH_LAYOUT AV_CH_LAYOUT_STEREO
#define AUDIO_RESAMPLE_SPEAKERS SPEAKERS_STEREO

#define AUDIO_RESAMPLE_AV_SAMPLE_FMT AV_SAMPLE_FMT_FLT
#define AUDIO_RESAMPLE_AUDIO_FORMAT AUDIO_FORMAT_FLOAT
#define AUDIO_RESAMPLE_SAMPLE_SIZE sizeof(float)

#define AUDIO_RESAMPLE_SAMPLE_RATE 44100
#define AUDIO_RESAMPLE_FRAME_SIZE \
	(AUDIO_RESAMPLE_CHANNELS * AUDIO_RESAMPLE_SAMPLE_SIZE)

struct audio_metadata {
	speaker_layout layout;
	audio_format format;
	int samples_per_sec;
	uint32_t frames;
};
