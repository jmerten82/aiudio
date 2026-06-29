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
    aiudio_add_example(ex_play_sine_device)      # plays to a real device (Core Audio, M2)
    aiudio_add_example(ex_device_probe)          # silent IOProc probe — objective M2 check
    aiudio_add_example(ex_capture_meter)         # live input level meter (M3)
    aiudio_add_example(ex_capture_to_ringbuffer) # capture → ring buffer → WAV (M3)
    aiudio_add_example(ex_duplex_probe)          # silent full-duplex probe (M4)
    aiudio_add_example(ex_duplex_passthrough)    # live in→out monitoring (M4)
    aiudio_add_example(ex_list_audio_processes)  # list tappable processes (M5, no capture)

    # M5 tap capture: embed NSAudioCaptureUsageDescription into the binary and
    # ad-hoc sign it, so macOS shows the audio-capture (purple-dot) TCC prompt
    # and tracks the grant (docs/70 §6). Without this the tap captures silence.
    aiudio_add_example(ex_tap_capture)
    target_link_options(ex_tap_capture PRIVATE
        "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
    set_source_files_properties(ex_tap_capture.cpp PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
    add_custom_command(TARGET ex_tap_capture POST_BUILD
        COMMAND codesign --force --sign - --identifier com.aiudio.examples.ex_tap_capture
                "$<TARGET_FILE:ex_tap_capture>"
        COMMENT "Ad-hoc signing ex_tap_capture (for the audio-capture TCC prompt)"
        VERBATIM)
endif()
