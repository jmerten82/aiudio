find_package(Threads REQUIRED)

# Build one example executable per .cpp, linking the aiudio-io library.
function(aiudio_add_example name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE aiudio::io Threads::Threads)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
endfunction()

aiudio_add_example(ex_ring_buffer)
aiudio_add_example(ex_render_callback)
aiudio_add_example(ex_conversions)
aiudio_add_example(ex_offline_render_wav)

if(APPLE)
    aiudio_add_example(ex_play_sine_device)  # plays to a real device (Core Audio, M2)
    aiudio_add_example(ex_device_probe)      # silent IOProc probe — objective M2 check
endif()
