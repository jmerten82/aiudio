"""Cross-clock multi-device (M9.6) at the Python boundary: CrossClockBridge.

One input device (master clock) + one output device on a SEPARATE clock, kept in sync via
the bridge's drift-compensated output path (ADR-0015 §4). Two MockBackends at different tick
cadences stand in for two physical clocks. Mirrors the C++ tests headlessly.
"""
from __future__ import annotations

SR = 48000.0


def _rig(aud, gain=1.0, ring=8192):
    g = aud.Graph()
    s, gn, k = g.add_source(0), g.add_gain(gain), g.add_sink(0)
    g.connect(s, 0, gn, 0)
    g.connect(gn, 0, k, 0)
    ex = aud.GraphExecutor()
    assert ex.compile(g, channels=1, sample_rate=SR, max_block=64)
    mgr = aud.MultiSourceManager(1, 1, 1, 64, 512)
    bridge = aud.CrossClockBridge(mgr, ex, 0, 0, SR, SR, 1, ring, 64)
    a, b = aud.MockBackend(), aud.MockBackend()
    assert bridge.attach_master(a, 1, 0, 64)
    assert bridge.attach_output(b, 0, 1, 64)
    return bridge, a, b


def test_dc_flows_across_clocks(aud):
    bridge, a, b = _rig(aud, gain=0.5)
    a.set_input_value(0.6)
    a.start()
    b.start()
    for _ in range(200):
        a.tick(64)
        b.tick(64)
    assert abs(b.captured_output(0) - 0.3) < 1e-3  # 0.6 → gain 0.5 → 0.3, across the boundary


def test_slow_output_clock_no_overrun(aud):
    ring = 8192
    bridge, a, b = _rig(aud, gain=1.0, ring=ring)
    a.set_input_value(0.4)
    a.start()
    b.start()
    b_acc, max_fill = 0.0, 0
    for step in range(30000):
        a.tick(64)
        b_acc += 64.0 * 0.998  # output clock 0.2% slow
        while b_acc >= 64.0:
            b.tick(64)
            b_acc -= 64.0
        if step > 4000:
            max_fill = max(max_fill, bridge.output_fill)
    assert bridge.output_overruns == 0   # ring never overflowed
    assert max_fill < ring
    assert bridge.output_ratio > 1.0     # servo sped consumption up


def test_fast_output_clock_settles(aud):
    bridge, a, b = _rig(aud, gain=1.0)
    a.set_input_value(0.4)
    a.start()
    b.start()

    def run(steps):
        b_acc = 0.0
        for _ in range(steps):
            a.tick(64)
            b_acc += 64.0 * 1.002  # output clock 0.2% fast
            while b_acc >= 64.0:
                b.tick(64)
                b_acc -= 64.0

    run(25000)
    under0 = bridge.output_underruns
    run(5000)
    assert bridge.output_underruns == under0  # no new starvation after convergence
    assert bridge.output_ratio < 1.0          # servo slowed consumption
