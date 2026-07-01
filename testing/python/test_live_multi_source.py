"""LiveMultiSource at the Python boundary (headless): N live sources on different clocks →
one multi-stream graph → a master output device. MockBackends stand in for the source + master
device clocks (deterministic, CI-safe); the real Core Audio path is a gated live test.
"""
from __future__ import annotations

SR = 48000.0


def _mix_engine(aud):
    g = aud.Graph()
    a0, a1 = g.add_source(0), g.add_source(1)
    mx, k = g.add_sum(2), g.add_sink(0)
    g.connect(a0, 0, mx, 0)
    g.connect(a1, 0, mx, 1)
    g.connect(mx, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    return aud.LiveMultiSource(ex, engine_rate=SR, channels=1, max_block=64)


def test_two_sources_mix_at_master(aud):
    lms = _mix_engine(aud)
    sA, sB, m = aud.MockBackend(), aud.MockBackend(), aud.MockBackend()
    assert lms.attach_source(sA, stream=0, source_rate=SR, block_size=64)
    assert lms.attach_source(sB, stream=1, source_rate=SR, block_size=64)
    assert lms.attach_master_output(m, out_stream=0, sample_rate=SR, block_size=64)
    assert lms.source_count == 2
    sA.set_input_value(0.5)
    sB.set_input_value(0.2)
    sA.start()
    sB.start()
    m.start()
    for _ in range(400):
        sA.tick(64)
        sB.tick(64)
        m.tick(64)
    assert abs(m.captured_output(0) - 0.7) < 2e-3        # 0.5 + 0.2, mixed onto the engine timeline
    assert lms.source_underruns(0) == 0


def test_source_on_different_clock_is_bounded(aud):
    ring = 8192
    lms = _mix_engine(aud)
    sA, sB, m = aud.MockBackend(), aud.MockBackend(), aud.MockBackend()
    lms.attach_source(sA, stream=0, source_rate=SR, block_size=64, ring_frames=ring)
    lms.attach_source(sB, stream=1, source_rate=44100.0, block_size=64, ring_frames=ring)  # 44.1k clock
    lms.attach_master_output(m, out_stream=0, sample_rate=SR, block_size=64)
    sA.set_input_value(0.5)
    sB.set_input_value(0.2)
    sA.start()
    sB.start()
    m.start()
    acc, max_fill = 0.0, 0
    for step in range(20000):
        sA.tick(64)
        acc += 64 * (44100.0 / SR)                       # source B on a slower (44.1k) clock
        while acc >= 64:
            sB.tick(64)
            acc -= 64
        m.tick(64)
        if step > 3000:
            max_fill = max(max_fill, lms.source_fill(1))
    assert lms.source_overruns(1) == 0                   # slower source: ring never overflowed
    assert max_fill < ring
    assert 0.87 < lms.source_ratio(1) < 0.95             # servo tracked 44100/48000 ≈ 0.919
    assert abs(m.captured_output(0) - 0.7) < 1e-2        # mix still ≈ 0.5 + 0.2
