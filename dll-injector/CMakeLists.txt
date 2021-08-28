project(dll-injector)

set(dll-injector_SOURCES
        dll-injector.cpp)

if(MSVC)
        add_compile_options($<IF:$<CONFIG:Debug>,/MTd,/MT>)
endif()

add_executable(dll-injector
        ${dll-injector_SOURCES})

target_link_libraries(dll-injector)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_output_suffix "64")
else()
        set(_output_suffix "32")
endif()

set_target_properties(dll-injector PROPERTIES FOLDER "plugins/obs-app-audio" OUTPUT_NAME "dll-injector${_output_suffix}")
set_property(TARGET obs-app-audio PROPERTY CXX_STANDARD 20)

install_obs_datatarget(dll-injector "obs-plugins/obs-app-audio")
