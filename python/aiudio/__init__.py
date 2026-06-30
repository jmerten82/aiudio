"""aiudio — AI-native digital audio processing framework (Python bindings).

Thin Python layer over the C++ graph engine. The real-time core stays in C++
(ADR-0002); this exposes the graph IR, the executor (with numpy I/O), and the
offline file backend so the research/ML layer and the agent can build, run, and
edit graphs.
"""

from ._aiudio import (  # noqa: F401
    Graph,
    GraphExecutor,
    MultiSourceManager,
    OfflineBackend,
    WavFormat,
)

__all__ = ["Graph", "GraphExecutor", "OfflineBackend", "WavFormat", "MultiSourceManager"]

# The live device backend is a Core Audio (macOS) feature — a *control* frontend over
# the real-time C++ audio thread (the audio thread never runs Python; ADR-0004). It is
# present only where the extension was built with it.
try:
    from ._aiudio import (  # noqa: F401
        AudioDeviceInfo,
        DeviceBackend,
        DuplexBackend,
        InputBackend,
        ProcessInfo,
        TapBackend,
    )

    __all__ += [
        "DeviceBackend", "AudioDeviceInfo",
        "InputBackend", "DuplexBackend", "TapBackend", "ProcessInfo",
    ]
except ImportError:  # pragma: no cover - non-macOS builds
    pass

__version__ = "0.0.1"
