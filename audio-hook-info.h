#pragma once
#include <media-io/audio-io.h>
#include <stdint.h>

// clang-format off

#define AUDIO_PIPE_NAME                 "AudioHook_Pipe"
#define AUDIO_PIPE_ASSUMED_TYPE         float

// clang-format on

struct pipe_metadata {
    uint64_t timestamp;
};
