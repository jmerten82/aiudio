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
aiudio_add_test(test_graph aiudio::graph)           # G1
aiudio_add_test(test_graph_nodes aiudio::graph)     # G1
aiudio_add_test(test_graph_executor aiudio::graph)  # G2

if(APPLE)
    aiudio_add_test(test_coreaudio_enumerate aiudio::io)  # device enumeration, no audio
endif()
