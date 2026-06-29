# Phase-0 testing strategy — C++ tests that live alongside the strategy doc
# (testing/README.md) rather than in tests/, because they verify cross-cutting
# guarantees (RT-safety) rather than a single unit. Built + run as part of the normal
# `ctest` suite (added from the root CMakeLists under AIUDIO_BUILD_TESTS).
find_package(Threads REQUIRED)

add_executable(test_rt_safety_alloc test_rt_safety_alloc.cpp)
target_link_libraries(test_rt_safety_alloc PRIVATE aiudio::graph Threads::Threads)
target_compile_options(test_rt_safety_alloc PRIVATE -Wall -Wextra)
add_test(NAME test_rt_safety_alloc COMMAND test_rt_safety_alloc)
