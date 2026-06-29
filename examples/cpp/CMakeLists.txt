find_package(Threads REQUIRED)

# Build one example executable per .cpp. The aiudio library to link is passed as
# extra args (e.g. aiudio::io or aiudio::graph) — linking exactly one avoids the
# macOS "duplicate libraries" warning from linking io both directly and via graph.
function(aiudio_add_example name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN} Threads::Threads)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
endfunction()

aiudio_add_example(ex_ring_buffer aiudio::io)
aiudio_add_example(ex_render_callback aiudio::io)
aiudio_add_example(ex_conversions aiudio::io)
aiudio_add_example(ex_offline_render_wav aiudio::io)
aiudio_add_example(ex_build_graph aiudio::graph)       # G1: build + validate a graph
aiudio_add_example(ex_run_graph_offline aiudio::graph)  # G2: compile + run via executor
aiudio_add_example(ex_render_file_offline aiudio::graph) # G4+M6: WAV → graph → WAV
aiudio_add_example(ex_graph_live_edit aiudio::graph)     # G5: live atomic schedule swap

if(APPLE)
    aiudio_add_example(ex_play_sine_device aiudio::io)      # plays to a device (M2)
    aiudio_add_example(ex_device_probe aiudio::io)          # silent IOProc probe (M2)
    aiudio_add_example(ex_capture_meter aiudio::io)         # live input meter (M3)
    aiudio_add_example(ex_capture_to_ringbuffer aiudio::io) # capture → ring → WAV (M3)
    aiudio_add_example(ex_duplex_probe aiudio::io)          # silent full-duplex probe (M4)
    aiudio_add_example(ex_duplex_passthrough aiudio::io)    # live in→out monitoring (M4)
    aiudio_add_example(ex_list_audio_processes aiudio::io)  # list tappable processes (M5)

    # G3 — drive a compiled graph live through the Core Audio duplex backend.
    # Link aiudio::graph (it pulls in aiudio::io + the Core Audio frameworks).
    aiudio_add_example(ex_graph_capture_probe aiudio::graph)    # silent, objective
    aiudio_add_example(ex_graph_live_passthrough aiudio::graph) # live monitoring (hands-on)

    # M5 tap capture: embed NSAudioCaptureUsageDescription into the binary and
    # ad-hoc sign it, so macOS shows the audio-capture (purple-dot) TCC prompt
    # and tracks the grant (docs/70 §6). Without this the tap captures silence.
    aiudio_add_example(ex_tap_capture aiudio::io)
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
