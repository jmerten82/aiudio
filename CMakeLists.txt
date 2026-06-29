cmake_minimum_required(VERSION 3.20)
project(aiudio
    VERSION 0.0.1
    DESCRIPTION "AI-native digital audio processing framework"
    LANGUAGES CXX)

# ---- Standard ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Export compile_commands.json for tooling (clang-tidy, IDEs).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Options ----
option(AIUDIO_BUILD_TESTS "Build aiudio tests" ON)
option(AIUDIO_BUILD_EXAMPLES "Build aiudio C++ examples" ON)
set(AIUDIO_SANITIZE "" CACHE STRING
    "Sanitizers to enable, e.g. 'address,undefined' or 'thread' (empty = none)")

if(AIUDIO_SANITIZE)
    message(STATUS "aiudio: sanitizers = ${AIUDIO_SANITIZE}")
    add_compile_options(-fsanitize=${AIUDIO_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${AIUDIO_SANITIZE})
endif()

# ---- Library: aiudio-io (M1) ----
# The I/O foundation: shared value types, the RenderCallback / AudioBackend
# contracts, the lock-free ring buffer, and sample-format conversions.
add_library(aiudio_io
    src/io/conversions.cpp)
add_library(aiudio::io ALIAS aiudio_io)

target_include_directories(aiudio_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(aiudio_io PUBLIC cxx_std_20)
target_compile_options(aiudio_io PRIVATE -Wall -Wextra -Wpedantic)

# macOS: the Core Audio HAL output backend (M2) and its frameworks.
if(APPLE)
    target_sources(aiudio_io PRIVATE
        src/io/coreaudio_backend.cpp         # M2 output backend
        src/io/coreaudio_input_backend.cpp   # M3 input backend
        src/io/coreaudio_duplex_backend.cpp) # M4 full-duplex backend
    target_link_libraries(aiudio_io PUBLIC
        "-framework CoreAudio"
        "-framework CoreFoundation")
endif()

# ---- Tests ----
if(AIUDIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- Examples ----
if(AIUDIO_BUILD_EXAMPLES)
    add_subdirectory(examples/cpp)
endif()
