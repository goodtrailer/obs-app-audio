project(audio-hook)

set(audio-hook_SOURCES
        audio-hook.cpp
        core-audio-capture.cpp)

if(MSVC)
        add_compile_options("$<IF:$<CONFIG:Debug>,/MTd,/MT>")
        set(audio-hook_PLATFORM_DEPS
                w32-pthreads)
endif()

add_library(audio-hook MODULE
        ${audio-hook_SOURCES}
        ${audio-hook_HEADERS})

target_include_directories(audio-hook PUBLIC
	"..")

target_link_libraries(audio-hook
        ${audio-hook_PLATFORM_DEPS}
        libobs
        ipc-util
        psapi)

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_output_suffix "64")
else()
        set(_output_suffix "32")
endif()

set_target_properties(audio-hook PROPERTIES FOLDER "plugins/obs-app-audio" OUTPUT_NAME "audio-hook${_output_suffix}")
set_property(TARGET obs-app-audio PROPERTY CXX_STANDARD 20)

install_obs_datatarget(audio-hook "obs-plugins/obs-app-audio")
