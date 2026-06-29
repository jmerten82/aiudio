# Python bindings (G6 / M8) — the nanobind extension module `_aiudio`.
nanobind_add_module(_aiudio aiudio_bindings.cpp)
target_link_libraries(_aiudio PRIVATE aiudio::graph)  # pulls in aiudio::io transitively
# Silence a known false-positive in nanobind's own headers (nb_func.h) under clang.
target_compile_options(_aiudio PRIVATE -Wno-array-bounds)

# For `pip install .` (scikit-build-core): place the module inside the aiudio
# package. Harmless for a plain CMake build.
install(TARGETS _aiudio LIBRARY DESTINATION aiudio)
