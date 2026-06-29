# examples/ — M0 I/O spikes

Throwaway Python spikes that prove macOS audio **permissions + device plumbing**
end-to-end before any C++ is written. They are milestone **M0** of the I/O layer
([`docs/71-io-layer-milestones.md`](../docs/71-io-layer-milestones.md)) and are
intentionally *not* part of the framework — the real I/O layer is the C++
`aiudio-io` library (M1+).

## Setup

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r examples/requirements.txt
```

`sounddevice` bundles PortAudio, which uses Core Audio on macOS. PortAudio does
**device I/O only** — it cannot do system/per-app process taps (that's why the
real layer is Core Audio in C++; see `docs/70-*`).

## The three spikes

| Script | Proves | Run |
|---|---|---|
| `m0_sine_out.py` | **output** | `python examples/m0_sine_out.py --device Kanto` |
| `m0_input_meter.py` | **input** + mic permission | `python examples/m0_input_meter.py --device Sennheiser` |
| `m0_passthrough.py` | **full duplex** (one clock) | `python examples/m0_passthrough.py --device-in Sennheiser --device-out Kanto` |

List devices first to see exact names/indices:

```bash
python examples/m0_sine_out.py --list-devices
```

## Offline self-tests (no audio device, no permissions)

Every spike has a `--self-test` that validates its DSP logic offline — handy for
CI and for checking the code without making noise or triggering TCC prompts:

```bash
for s in sine_out input_meter passthrough; do python examples/m0_$s.py --self-test; done
```

## Notes / gotchas

- **First run prompts for microphone access** (the input + passthrough spikes).
  The prompt attaches to the host (Terminal / your Python). See `docs/70-*` §6.
- **Use headphones for passthrough** — open speakers + open mic will feed back.
- Acceptance criteria for M0 are in `docs/71-io-layer-milestones.md` §3.
