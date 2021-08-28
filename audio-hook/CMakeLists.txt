project(audio-hook)

set(audio-hook_SOURCES
        audio-hook.cpp
        core-audio-capture.cpp)

add_library(audio-hook MODULE
        ${audio-hook_SOURCES})

target_include_directories(audio-hook PUBLIC
	"..")

target_compile_options(audio-hook PUBLIC
        "$<IF:$<CONFIG:Debug>,/MTd,/MT>")

target_link_libraries(audio-hook
        w32-pthreads
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
