find_package(Threads REQUIRED)

# Register a single-file test executable as a CTest test.
function(aiudio_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE aiudio::io Threads::Threads)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

aiudio_add_test(test_ring_buffer)
aiudio_add_test(test_conversions)

if(APPLE)
    aiudio_add_test(test_coreaudio_enumerate)  # device enumeration, no audio
endif()
