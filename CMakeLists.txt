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
option(AIUDIO_BUILD_PYTHON "Build the Python bindings (requires nanobind)" OFF)
set(AIUDIO_SANITIZE "" CACHE STRING
    "Sanitizers to enable, e.g. 'address,undefined' or 'thread' (empty = none)")

if(AIUDIO_SANITIZE)
    message(STATUS "aiudio: sanitizers = ${AIUDIO_SANITIZE}")
    add_compile_options(-fsanitize=${AIUDIO_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${AIUDIO_SANITIZE})
endif()

# ---- Library: aiudio-io (M1) ----
# The I/O foundation: shared value types, the RenderCallback / AudioBackend
# contracts, the lock-free ring buffer, sample-format conversions, WAV file I/O,
# and the offline (file) backend (M6).
add_library(aiudio_io
    src/io/conversions.cpp
    src/io/wav_file.cpp              # M6: WAV reader/writer
    src/io/offline_backend.cpp       # M6: offline (file) backend
    src/io/mock_backend.cpp          # M9.4: deterministic test backend
    src/io/resampling_source.cpp)    # M9.5: off-clock source (ring + resampler + drift servo)
add_library(aiudio::io ALIAS aiudio_io)

target_include_directories(aiudio_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(aiudio_io PUBLIC cxx_std_20)
target_compile_options(aiudio_io PRIVATE -Wall -Wextra -Wpedantic)

# macOS: the Core Audio backends and their frameworks. The process-tap backend
# (M5) is Objective-C++ (.mm, for CATapDescription) and needs Foundation.
if(APPLE)
    target_sources(aiudio_io PRIVATE
        src/io/coreaudio_backend.cpp            # M2 output backend
        src/io/coreaudio_input_backend.cpp      # M3 input backend
        src/io/coreaudio_duplex_backend.cpp     # M4 full-duplex backend
        src/io/coreaudio_process_tap_backend.mm) # M5 process-tap backend (Obj-C++)
    target_link_libraries(aiudio_io PUBLIC
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Foundation")
endif()

# ---- Library: aiudio-graph (G1) ----
# The graph IR + node contract (ADR-0009). Pure C++; depends on aiudio-io for the
# AudioBuffer / TimeInfo types. Construction + validation now; execution is G2.
add_library(aiudio_graph
    src/graph/graph.cpp
    src/graph/graph_executor.cpp          # G2: compile + eager executor
    src/graph/multi_source_manager.cpp)   # M10: multi-source manager
add_library(aiudio::graph ALIAS aiudio_graph)
target_include_directories(aiudio_graph PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(aiudio_graph PUBLIC aiudio::io)
target_compile_features(aiudio_graph PUBLIC cxx_std_20)
target_compile_options(aiudio_graph PRIVATE -Wall -Wextra -Wpedantic)

# ---- Tests ----
if(AIUDIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(testing/cpp)  # Phase-0 strategy: cross-cutting RT-safety tests
endif()

# ---- Examples ----
if(AIUDIO_BUILD_EXAMPLES)
    add_subdirectory(examples/cpp)
endif()

# ---- Python bindings (G6 / M8) ----
if(AIUDIO_BUILD_PYTHON)
    find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
    # Auto-discover nanobind via the interpreter (works for both a manual CMake
    # build and the scikit-build-core `pip install .` flow).
    if(NOT nanobind_DIR)
        execute_process(COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
                        OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_DIR)
    endif()
    find_package(nanobind CONFIG REQUIRED)
    add_subdirectory(bindings)
endif()
