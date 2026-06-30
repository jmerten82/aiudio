#!/usr/bin/env bash
# Phase-0 test runner — one entry point for the strategy in testing/README.md.
#
#   testing/run.sh                 # headless: C++ build+ctest, sanitizers, pip install, pytest
#   testing/run.sh --no-sanitize   # skip the (slower) TSan/ASan passes
#   testing/run.sh --live          # also run the live RT device layer (macOS + a device)
#
# Activate your venv first (the Python steps use `python` on PATH; override with PYTHON=).
set -euo pipefail
cd "$(dirname "$0")/.."  # repo root

LIVE=0
SANITIZE=1
PY="${PYTHON:-python}"
for arg in "$@"; do
    case "$arg" in
        --live) LIVE=1 ;;
        --no-sanitize|--quick) SANITIZE=0 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

section() { printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }

section "C++ — build + ctest (unit, integration, golden, concurrency, RT-safety)"
cmake -S . -B build >/dev/null
cmake --build build -j
ctest --test-dir build --output-on-failure

if [ "$SANITIZE" = 1 ]; then
    section "C++ — ThreadSanitizer (control plane + live edit: race-free)"
    cmake -S . -B build-tsan -DAIUDIO_SANITIZE=thread >/dev/null
    cmake --build build-tsan -j --target test_graph_control test_graph_live_edit test_multi_source_manager
    ctest --test-dir build-tsan -R 'test_graph_control|test_graph_live_edit|test_multi_source_manager' --output-on-failure

    section "C++ — Address/UB Sanitizer (full suite)"
    cmake -S . -B build-asan -DAIUDIO_SANITIZE=address,undefined >/dev/null
    cmake --build build-asan -j
    ctest --test-dir build-asan --output-on-failure
fi

section "Python — install package + dev deps"
"$PY" -m pip install -q .
"$PY" -m pip install -q -r testing/python/requirements-dev.txt

section "Python — lint (ruff)"
ruff check python bindings examples testing

section "Python — headless pytest (live device tests auto-skip)"
"$PY" -m pytest testing/python bindings

if [ "$LIVE" = 1 ]; then
    section "Python — LIVE RT device layer (real output device)"
    AIUDIO_LIVE=1 "$PY" -m pytest testing/python -m live -v   # all live-marked tests (device + input)
fi

section "ALL TESTS PASSED"
