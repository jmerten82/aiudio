# 70 — Concrete Plan: Tapping Local Audio Sources on macOS

How aiudio captures live audio from this Mac — input devices, **system output**,
and **per-application audio** — and feeds it into the graph engine (`50-*`).

> **Target machine (verified 2026-06-28):** macOS **26.5.1** (Tahoe), Darwin
> 25.5.0, Apple clang 21, Swift 6.3.3, Xcode **Command Line Tools only** (no full
> Xcode), Homebrew 6.0.2, Python 3.14. **`cmake` not installed.** **No
> third-party virtual audio device** (only Zoom/Teams app drivers). Because this
> is macOS 26, **every modern capture API is available**, including the native
> Core Audio process-tap API (macOS 14.4+).

---

## 1. Source taxonomy — what "local audio sources" means

| # | Source | Example on this Mac | Live? |
|---|---|---|---|
| A | **Input/capture devices** | Sennheiser Profile (default in), eMeet webcam, MacBook mic, iPhone mic | yes |
| B | **System audio output (whole)** | everything playing → Kanto ORA4 (default out) | yes |
| C | **Per-application audio** | just Spotify / just a browser tab / just a DAW | yes |
| D | **Audio files** | wav/aiff/flac/mp3 on disk | offline |
| E | **Aggregate / multi-device** | several inputs combined | yes |

A, D, E are easy and well-trodden. **B and C are the interesting ones** — macOS
has **no native loopback input device**, so capturing output requires one of the
three approaches in §3.

---

## 2. The API decision matrix

| Source | Recommended API | Permission (TCC) | Why |
|---|---|---|---|
| Device enumerate/select | **Core Audio HAL** (`AudioObjectGetPropertyData`, `kAudioHardwarePropertyDevices`) | none | canonical device list |
| A — input/mic (RT) | **Core Audio HAL IOProc** (`AudioDeviceCreateIOProcID` + `AudioDeviceStart`) | Microphone (`NSMicrophoneUsageDescription`) | lowest-latency, RT-safe, fits the C++ core |
| A — input/mic (easy) | **AVAudioEngine** (`inputNode.installTap`) or Python **sounddevice** | Microphone | fastest to prototype |
| B — system output | **Core Audio Process Tap** (global tap) | **Audio Capture** (`NSAudioCaptureUsageDescription`) | native, low-latency, no virtual device |
| C — per-app output | **Core Audio Process Tap** (per-PID tap) | Audio Capture | the killer feature — tap one process |
| B/C — alternative | **ScreenCaptureKit** (`SCStreamConfiguration.capturesAudio`) | **Screen Recording** | use if you also want video/screen, or prefer Swift |
| B — zero-code today | **BlackHole** virtual device + Multi-Output | (device, no TCC) | unblock experiments immediately |
| D — files | **AVAudioFile** / **ExtAudioFile** (C++) or **libsndfile** | none | offline source nodes |

---

## 3. The three ways to capture output (B/C), ranked for aiudio

### ⭐ Primary — Core Audio **Process Taps** (macOS 14.4+) — *native, do this*
The modern Apple-sanctioned way to capture system **or per-process** output with
**no virtual device and no kernel extension**. Lowest latency, pure-audio, and it
plugs straight into the same HAL aggregate-device model the rest of aiudio's C++
capture layer uses. **This is the right long-term path for the framework.**

**Mechanism (the call sequence):**
1. *(per-app only)* Translate PID → process `AudioObjectID` via
   `kAudioHardwarePropertyTranslatePIDToProcessObject` on the system object.
2. Build a `CATapDescription` — global (whole system) or a process list;
   stereo/mono mixdown; exclusive/inclusive; mute behavior.
3. `AudioHardwareCreateProcessTap(desc, &tapID)` → a tap `AudioObjectID`.
4. `AudioHardwareCreateAggregateDevice(dict, &aggID)` where the dict carries
   `kAudioAggregateDeviceTapListKey` (referencing the tap UID via
   `kAudioSubTapUIDKey`) + `kAudioAggregateDeviceTapAutoStartKey` +
   `kAudioAggregateDeviceIsPrivateKey`.
5. `AudioDeviceCreateIOProcID(aggID, ioProc, …)` + `AudioDeviceStart` → the
   IOProc fires with the tapped audio. Copy into the **lock-free ring buffer**.
6. Teardown: `AudioDeviceStop` → destroy IOProcID → destroy aggregate device →
   `AudioHardwareDestroyProcessTap`.

**Permission:** add `NSAudioCaptureUsageDescription` to the binary's Info.plist;
first read triggers a TCC prompt; indicator is the **purple** dot (not the mic
orange dot).

**Reference implementations (study these):**
- `insidegui/AudioCap` — the de-facto sample for system audio on macOS 14.4+.
- Apple: *"Capturing system audio with Core Audio taps."*
- maven.de — *"CoreAudio Taps for Dummies"* (the missing manual).

### Alternative — ScreenCaptureKit
`SCStream` + `SCStreamConfiguration.capturesAudio = true` (system audio, macOS
13+), `.captureMicrophone` (macOS 15+), `.excludesCurrentProcessAudio`. Per-app
audio via an `SCContentFilter` scoped to that app's windows.
**Choose this only if** you also want screen/video capture, or you want a pure
Swift/AVFoundation path. **Downsides for an audio framework:** Screen-Recording
permission (heavier than audio-capture), higher-level/AV-oriented, and
self-signed apps "frequently fail to acquire this permission" on recent macOS.

### Fallback — BlackHole virtual device (*use today to unblock*)
`brew install blackhole-2ch`. Create a **Multi-Output Device** (real output +
BlackHole) in *Audio MIDI Setup*, set it as system output, then capture
**BlackHole** as an ordinary input device (works with PortAudio/`sounddevice`
**today**, no custom code). **Downsides:** manual routing, you stop hearing audio
unless you multi-out, no per-app granularity, adds a user-install dependency.
Great for **M0 experiments**, not the shipping design.

---

## 4. Recommended architecture for aiudio

A **C++ Core Audio capture module** in the real-time core, surfaced to Python —
exactly matching the threading model already designed in `50-architecture-patterns.md`
(RT callback → lock-free ring buffer → consumers).

```
   Core Audio (HAL / Process Tap IOProc)         ← runs on a RT audio thread
            │  writes (no locks, no alloc)
            ▼
   ┌──────────────────────┐
   │  SPSC lock-free ring  │  (one per source)
   └──────────────────────┘
        │                 │
        ▼                 ▼
  aiudio graph        Python (nanobind)
  CaptureSource       list_devices() / open_input(dev)
  node (50-*)         open_system_tap() / open_process_tap(pid)
  → RT executor       → numpy blocks / callback
```

**Public surface (the deliverable):**
```
aiudio.audio.list_devices() -> [DeviceInfo]      # HAL enumeration
aiudio.audio.open_input(device_id, sr, block)    # source A  (mic/line)
aiudio.audio.open_system_tap(sr, block)          # source B  (whole system)
aiudio.audio.open_process_tap(pid|bundle_id)     # source C  (one app)
aiudio.audio.open_file(path)                     # source D  (offline)
# each yields a stream of float32 blocks + timestamps, and/or is usable
# directly as a CaptureSource node in the graph engine.
```

**Why C++-first, not Python-first:** PortAudio/`sounddevice` only does **device
I/O** — it cannot do system/process taps. The native tap API is C/Core Audio
only. Building the capture layer in the C++ core (a) gives RT-safety, (b) unifies
device + tap capture behind one ring-buffer contract, (c) feeds the graph engine
without a Python round-trip on the audio thread.

---

## 5. Milestones (with acceptance criteria)

> **M0 is doable in the next ~30 min** to prove the pipeline; M1–M2 build the real
> native layer; M3–M4 wire it into aiudio.

| Milestone | Deliverable | Acceptance criteria | Est. |
|---|---|---|---|
| **M0 — hear bytes (input)** | Python `sounddevice` mic spike → print RMS / write wav | Captures from Sennheiser/MacBook mic; mic TCC prompt accepted | 0.5 d |
| **M0.5 — hear bytes (system)** | `brew install blackhole-2ch` + Multi-Output → capture BlackHole | System audio (e.g. a browser tab) captured to wav via `sounddevice` | 0.5 d |
| **M1 — C++ HAL input core** | CMake C++ module: device enumeration + input IOProc → SPSC ring buffer; tiny test exe | Lists all devices; captures default input glitch-free at 48 kHz/128; xrun counter = 0 at idle | 3–4 d |
| **M2 — native taps** | Add system + per-PID **Process Tap** capture via aggregate device; signed binary w/ `NSAudioCaptureUsageDescription` | Captures whole-system audio **and** a chosen app (e.g. Spotify) with no BlackHole; purple TCC indicator shows | 4–6 d |
| **M3 — Python bindings** | nanobind module exposing the §4 surface; numpy block callback | `open_process_tap("com.spotify.client")` yields audio in a Jupyter cell | 2–3 d |
| **M4 — graph integration** | `CaptureSource` node in the graph IR (`50-*`); RT executor consumes the ring buffer | A 2-node graph `tap → meter` runs real-time inside aiudio | 2–3 d |
| **M5 — robustness** | Sample-rate conversion, device hot-plug, multi-source sync, record sinks, format negotiation | Survives unplugging the USB interface mid-capture; SR mismatch handled | ongoing |

---

## 6. Permissions & packaging — the #1 thing that trips people up

TCC (privacy) is keyed to the **binary's identity**, which is awkward for CLI
tools and Python extensions:

- **Microphone (M0–M1):** `NSMicrophoneUsageDescription`. Running a script from
  **Terminal** attaches the prompt to Terminal (or the Python.framework binary) —
  fine for prototyping. First run shows *"Terminal wants to access the
  microphone."*
- **Audio capture / taps (M2+):** `NSAudioCaptureUsageDescription` must live in
  **the capturing binary's Info.plist**. A bare CLI/dylib has none → no prompt →
  silent failure. **Mitigations:**
  - Package the capture helper as a **signed `.app`** (or `.framework`) with a
    proper Info.plist, *or*
  - For a CLI, embed the plist at link time:
    `-Wl,-sectcreate,__TEXT,__info_plist,Info.plist` and **codesign** it
    (`codesign --force --sign - --entitlements …`). Ad-hoc signing works for
    local dev.
  - Self-signed apps sometimes don't get the prompt on recent macOS — grant
    manually in **System Settings → Privacy & Security** if needed.
- **Screen Recording (only if using ScreenCaptureKit):** heavier, TCC-only (no
  entitlement grants it); self-signed apps frequently fail to prompt.
- **Full Xcode not required**, but it makes signed-bundle + entitlements builds
  much easier than CLT alone. The native-tap helper is the one place you may want
  full Xcode (or careful manual `codesign`).

---

## 7. Do this first (the next hour)

```bash
# 1. Toolchain gaps
brew install cmake
python3 -m pip install sounddevice soundfile numpy   # PortAudio-based spike

# 2. (optional) zero-code system-audio capture today
brew install blackhole-2ch
#   then: Audio MIDI Setup → create Multi-Output Device (Kanto ORA4 + BlackHole),
#   set as system output; capture "BlackHole 2ch" as an input below.
```

```python
# M0 spike — capture the default input, print level (mic TCC prompt on first run)
import sounddevice as sd, numpy as np
print(sd.query_devices())                       # see Sennheiser / BlackHole / etc.
def cb(indata, frames, t, status):
    print(f"\rRMS {np.sqrt((indata**2).mean()):.4f}", end="")
with sd.InputStream(channels=1, samplerate=48000, blocksize=128, callback=cb):
    sd.sleep(10_000)                            # 10 s
```

This confirms permissions + device plumbing end-to-end before any C++ is written.
Then start **M1** (the C++ HAL core) — that's the first real piece of aiudio's
capture layer.

---

## 8. How this maps back to the framework

- The ring-buffer / RT-thread design is **already specified** in
  `50-architecture-patterns.md` §4 (audio thread is sacred; lock-free hand-off).
  This plan is its **input-side instantiation**.
- A `CaptureSource` is just another **graph node** honoring the node contract
  (`50-*` §2) — `process(block)` pulls from the ring buffer; it advertises
  `realtime_capable = true`, its latency, and sample-rate.
- Per-process taps (C) are uniquely valuable for the **agent/copilot** (`40-*`):
  "clean up the audio from my video call" = tap that app's process, run a
  denoiser node, no routing setup by the user.

---

## 9. References

- **Core Audio process taps:**
  - Apple — *Capturing system audio with Core Audio taps*:
    https://developer.apple.com/documentation/CoreAudio/capturing-system-audio-with-core-audio-taps
  - `AudioHardwareCreateProcessTap`:
    https://developer.apple.com/documentation/coreaudio/audiohardwarecreateprocesstap(_:_:)
  - `insidegui/AudioCap` (sample, macOS 14.4+): https://github.com/insidegui/AudioCap
  - *CoreAudio Taps for Dummies*: https://www.maven.de/2025/04/coreaudio-taps-for-dummies/
- **ScreenCaptureKit:**
  - *Capturing screen content in macOS*:
    https://developer.apple.com/documentation/ScreenCaptureKit/capturing-screen-content-in-macos
  - `capturesAudio`:
    https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/capturesaudio
- **Core Audio HAL / AVAudioEngine:** Apple *Core Audio* & *AVFAudio* docs.
- **BlackHole** (virtual device): https://github.com/ExistentialAudio/BlackHole
- **PortAudio / python-sounddevice:** https://python-sounddevice.readthedocs.io/

> **Provenance:** API surface, version floors (taps 14.4+, SCK mic 15+), and
> permission keys (`NSAudioCaptureUsageDescription`, Screen Recording) verified
> via web search 2026-06-28. Core Audio HAL/IOProc, AVAudioEngine, BlackHole, and
> PortAudio details are established background — confirm exact symbols against the
> SDK headers when implementing.
