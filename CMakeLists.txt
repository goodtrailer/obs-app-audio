project(obs-app-audio)

if(MSVC)
        set(obs-app-audio_PLATFORM_DEPS
                w32-pthreads)
endif()

find_package(FFmpeg REQUIRED
        COMPONENTS avutil avformat swresample)
include_directories(${FFMPEG_INCLUDE_DIRS})

set(obs-app-audio_HEADERS
        audio-helpers.h
        audio-hook-info.h
)

set(obs-app-audio_SOURCES
        app-audio-capture.cpp
        audio-helpers.cpp)

add_library(obs-app-audio MODULE
        ${obs-app-audio_SOURCES}
        ${obs-app-audio_HEADERS})

target_link_libraries(obs-app-audio
        ${obs-app-audio_PLATFORM_DEPS}
        libobs
        ipc-util
        psapi
        ${FFMPEG_LIBRARIES})

target_compile_definitions(obs-app-audio PUBLIC _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)

set_target_properties(obs-app-audio PROPERTIES FOLDER "plugins/obs-app-audio")
set_property(TARGET obs-app-audio PROPERTY CXX_STANDARD 20)

install_obs_plugin_with_data(obs-app-audio data)

add_subdirectory(audio-hook)
add_subdirectory(dll-injector)
