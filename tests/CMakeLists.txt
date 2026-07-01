find_package(Threads REQUIRED)

# Register a single-file test executable as a CTest test. The aiudio library to
# link is passed as extra args (aiudio::io or aiudio::graph) — linking exactly one
# avoids the macOS "duplicate libraries" warning (io is pulled in via graph).
function(aiudio_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN} Threads::Threads)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

aiudio_add_test(test_ring_buffer aiudio::io)
aiudio_add_test(test_conversions aiudio::io)
aiudio_add_test(test_resampler aiudio::io)          # M9.3 boundary sample-rate conversion
aiudio_add_test(test_drift aiudio::io)              # M9.5 drift compensation (servo + source)
aiudio_add_test(test_channel_map aiudio::io)        # M9.2 device<->graph channel mapping
aiudio_add_test(test_wav_file aiudio::io)            # M6
aiudio_add_test(test_graph aiudio::graph)           # G1
aiudio_add_test(test_graph_edit aiudio::graph)      # graph edits: disconnect + removeNode (tombstone)
aiudio_add_test(test_graph_nodes aiudio::graph)     # G1
aiudio_add_test(test_graph_executor aiudio::graph)  # G2
aiudio_add_test(test_graph_meter aiudio::graph)     # G3
aiudio_add_test(test_biquad_node aiudio::graph)     # G4 (+ peaking/shelf EQ family)
aiudio_add_test(test_dsp_nodes aiudio::graph)       # Tier-1 nodes: shaper/osc/noise/pan/mix/route/delay
aiudio_add_test(test_dynamics aiudio::graph)        # Tier-1 dynamics: compressor + gate
aiudio_add_test(test_parametric_eq aiudio::graph)   # multi-band EQ (cascaded biquad sections)
aiudio_add_test(test_offline_render aiudio::graph)  # G4 + M6 golden file→graph→file
aiudio_add_test(test_graph_live_edit aiudio::graph) # G5 atomic schedule swap
aiudio_add_test(test_graph_control aiudio::graph)   # G7 control plane: param command queue
aiudio_add_test(test_graph_multistream aiudio::graph) # G10 multi-stream executor
aiudio_add_test(test_graph_channels aiudio::graph)  # G8 per-port channel counts
aiudio_add_test(test_graph_latency aiudio::graph)   # G9 latency + delay compensation (PDC)
aiudio_add_test(test_graph_xrun aiudio::graph)      # M9.1 xrun/underrun policy + telemetry
aiudio_add_test(test_multi_source_manager aiudio::graph)  # M10 multi-source manager
aiudio_add_test(test_master_clock aiudio::graph)    # M10 live adapter + M9.4 mock/hot-plug
aiudio_add_test(test_cross_clock aiudio::graph)     # M9.6 cross-clock multi-device (bridge)
aiudio_add_test(test_live_multi_source aiudio::graph)  # C1/C2: N off-clock sources → graph → master

if(APPLE)
    aiudio_add_test(test_coreaudio_enumerate aiudio::io)  # device enumeration, no audio
    aiudio_add_test(test_coreaudio_disconnect aiudio::io) # M9.4 HAL device-died listener wiring
endif()
