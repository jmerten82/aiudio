# 77 — The Hard Problem of Combining Multiple Audio Inputs and Outputs

> **Last updated:** 2026-06-29 · An explainer on *why* merging several audio inputs and
> outputs inside one pipeline is genuinely difficult — the independent hazards, how they
> compound, why naïve approaches fail, and the architectural principles that tame them.
> Companion to the [multi-source I/O plan](76-multi-source-io-roadmap.md) and the
> [digital-audio-encoding primer](../theory/73-digital-audio-encoding.md).
>
> The audio-engineering facts here are established domain knowledge; references to
> **aiudio**'s own design are ✓ grounded in this repo (ADR-0004/0005/0008/0009).

---

## 0. The deceptive simplicity

On paper, combining audio sources is addition. Two microphones? Add the samples. Mic plus a
backing track? Add them. Send the mix to two pairs of speakers? Copy the buffer twice. A
first-year programmer can write `out[i] = a[i] + b[i]` and it compiles.

It also doesn't work — not for real devices, not in real time. The reason is that digital
audio is not a stream of numbers you can combine at will; it is a **hard-real-time signal
sampled against a physical clock**, and the moment you have *more than one* source or sink,
you have more than one clock, more than one deadline, more than one failure mode, and more
than one thread — none of which line up by default. "Multiple" doesn't add difficulty
linearly; it multiplies several already-hard constraints and then makes them interact.

This article walks the hazards one at a time, shows how they compound, and ends with the
design principles that make multi-I/O tractable.

---

## 1. Why the naïve mental model breaks

The intuitive picture is a mixing desk: wires in, a sum, wires out. The picture is wrong in
three ways that matter:

1. **The wires are not synchronous.** Each input arrives on its *own* clock and its *own*
   schedule; there is no global "now" at which `a[i]` and `b[i]` are the same instant.
2. **There is a referee with a stopwatch.** The audio hardware demands a full buffer every
   few milliseconds, on time, forever. The sum has to happen *inside that window*, every
   window, or you hear it fail.
3. **The wires can be yanked out mid-song.** USB devices unplug, Bluetooth drops, the user
   changes the default output. A real pipeline must survive that without a crash or a stall.

Strip those away and `out = a + b` is fine. Keep them — i.e. use real hardware — and each
becomes a sub-problem with its own literature. The rest of this article is those
sub-problems.

---

## 2. The tyranny of the deadline (the real-time constraint)

Audio output is a conveyor belt that never stops. At 48 kHz with a 128-frame buffer, the
device's callback fires every **2.67 ms** and *must* return a full block of samples. Late by
one microsecond and the DAC has nothing to convert — you get a gap, heard as a click or pop.
This is an **xrun** (buffer under/overrun).

That single fact forbids, *on the audio thread*, everything that might take an unbounded or
unpredictable amount of time: heap allocation, locks/mutexes, system calls, file/network
I/O, logging, exceptions, garbage collection, and — critically for a Python-ML framework —
the Python interpreter itself (the GIL). (This is aiudio's "the audio thread is sacred"
invariant, ✓ ADR-0004.)

Now make it *multiple*. With N inputs and M outputs:
- there is **more work to finish inside the same 2.67 ms** (mix N, route to M);
- the budget is set by the **worst-case** path, not the average;
- and the work must still be allocation-free and lock-free, which is exactly the discipline
  that's hardest to maintain as the pipeline grows.

The deadline is the backdrop to every other difficulty below: each "solution" (resampling,
drift correction, re-blocking) costs CPU *on that same thread*, and must fit the budget or
it is not a solution.

---

## 3. Many clocks, one timeline (the central difficulty)

This is the problem that surprises newcomers and humbles veterans.

A digital audio device samples against a **physical crystal oscillator**. "48 kHz" is a
*nominal* rate; the actual rate of any given device is 48 kHz ± a few parts per million, and
it **changes with temperature and time**. Two devices nominally at 48 kHz are really at,
say, 47999.7 Hz and 48000.4 Hz. Over a minute that's tens of samples of **drift**; over an
hour, thousands.

Consequences when you combine sources from different devices:
- Their sample streams are **not aligned** and slip relative to each other continuously.
- A ring buffer fed by device A and drained against device B's clock will **slowly fill or
  empty** until it overflows (overrun) or empties (underrun) — a periodic glitch whose
  spacing depends on the drift.
- You cannot "just interleave" two device streams; there is no shared notion of which sample
  of A coincides with which sample of B.

This is *the* reason full-duplex "open the mic and the speakers and pass through" is hard
when the mic and speakers are different devices: independent clocks drift, so the round-trip
is undefined and glitches (✓ this is the exact scenario ADR-0008 confronts on the dev box —
a Sennheiser input and Kanto output are different Core Audio devices).

There are only two real cures, and pro audio uses both:

1. **Make them share one clock.** Run everything off a *single* device's clock, or have the
   OS build an **aggregate device** that spans several sub-devices with one master clock and
   hardware/driver **drift compensation** on the others (macOS Core Audio, JACK, and ASIO
   all have versions of this). Pro studios solve it in hardware with **word clock** or
   digital sync (AES, ADAT) distributing one master clock to every box. (✓ aiudio's
   full-duplex backend creates a private aggregate device with the output as clock master,
   `src/io/coreaudio_duplex_backend.cpp`.)
2. **Resample onto a master timeline.** When devices can't share a clock, bring each
   off-clock source onto the engine's clock with **adaptive sample-rate conversion** driven
   by a control loop that watches the source's ring fill-level and nudges the resampling
   ratio to hold it steady. This is **drift compensation in software** — a feedback control
   problem layered on a DSP problem, and one of the genuinely hard things in audio
   middleware.

Everything else in this article is tractable engineering. *This* is the part that turns
"combine some audio" into a real project.

---

## 4. Heterogeneity: rates, formats, channels, blocks

Even setting drift aside, real sources rarely agree on representation:

- **Sample rate.** A USB mic at 44.1 kHz, an interface at 48 kHz, a VoIP stream at 16 kHz, a
  neural model that wants 22.05 kHz. To combine them they must be brought to one internal
  rate — i.e. **resampled**. And resampling between, say, 44.1 and 48 kHz is a ratio of
  **147:160**, which needs a real polyphase FIR (or a good library): it costs CPU, adds
  **latency** (filter group delay), and — done cheaply — adds aliasing/imaging artifacts.
- **Bit depth / encoding.** int16, int24-packed, int32, float32; each must be converted to
  the engine's working format (aiudio uses planar float32, ✓ `docs/theory/73`). Conversions are
  cheap but must be exact and consistent (clipping, dither, byte order).
- **Interleaving.** Devices hand you interleaved frames (`L R L R …`); most engines process
  **planar** (per-channel) buffers. De/re-interleaving at every edge.
- **Channel count & layout.** Mono, stereo, 5.1, a 32-channel interface. Combining sources
  with different channel counts forces decisions — upmix, downmix, channel selection, a
  routing matrix — and an engine whose buffers are a *fixed* channel width can't even
  *represent* a source of a different width without a change to its core (✓ exactly the
  "per-port channel count" extension aiudio's `docs/pipeline/76` Phase A scopes).
- **Block size.** One device delivers 512-frame buffers, another 256, a software tap a
  variable size, a plugin host whatever it likes. The engine wants a fixed block, so you
  must **re-block** (accumulate and re-chunk), which adds buffering and latency.

Each conversion is individually easy; the difficulty is that combining sources means doing
*all of them, consistently, at every edge, inside the deadline*, and that resampling in
particular drags latency into the picture — which is the next problem.

---

## 5. Latency: alignment, not just size

Every path through the system has a **latency**: device input buffering, the resampler's
group delay, processing blocks, output buffering. These differ per source and per path.

For combining, the killer is not the *amount* of latency but the **misalignment** of it.
If you sum two versions of a signal that are a few samples apart — a mic and its delayed
duplicate, two mics on one drum, a dry path and a processed return — you don't get a louder
signal, you get **comb filtering**: periodic notches across the spectrum, audible as
hollowness, phasing, or flam. Multi-mic recording lives or dies on sample-accurate
alignment.

So a serious multi-I/O engine needs:
- **Per-path latency reporting** — every source, resampler, and node declares how many
  frames it delays the signal. (✓ aiudio's node contract has no latency field *yet* — it's
  the G9 prerequisite in `docs/pipeline/76`.)
- **Delay compensation** — the engine delays the *shorter* paths so everything recombines in
  phase. DAWs call this **PDC (plug-in delay compensation)**; getting it right across a graph
  with feedback-free but latency-varying branches is fiddly bookkeeping.
- A finite **round-trip budget** for live use (monitoring needs ≲ 10–15 ms total), which
  every added buffer and resampler eats into.

Latency, in other words, isn't a number you minimize once; it's a property you must *track
and equalize* across every combined path.

---

## 6. Thread topology and the lock-free hand-off

Multiple devices means **multiple OS audio callbacks**, each on its **own real-time thread**,
each firing on its own device's clock. Add the control/UI thread, and off-thread workers
(recorders, file readers, neural inference, a Python consumer). Audio data has to move
between these threads **without ever taking a lock** (a lock on the audio thread can block
unboundedly → xrun).

The standard tool is the **wait-free ring buffer** — but a ring buffer is
**single-producer/single-consumer (SPSC)**. That constraint is load-bearing: SPSC is what
makes it lock-free and cheap. It also means **you cannot merge multiple producers into one
ring**. So N sources require **N rings**, each crossing from its device thread to the engine,
and the engine reads N rings and combines them *after* the boundary (✓ ADR-0008 §2: "each
off-clock source gets its own SPSC ring buffer… we never feed one ring from multiple
producers").

Coordinating N device callbacks + N rings + one engine clock + several consumers is a
genuine concurrency-systems problem on top of the DSP:
- **memory ordering** (acquire/release on every ring index, or you get torn reads);
- **false sharing** (producer and consumer cursors must sit on separate cache lines, or the
  two threads thrash each other's caches — ✓ aiudio's `RingBuffer` pads them);
- **priority inversion** (a low-priority thread holding something the audio thread needs);
- **lifetime** (who owns the ring when a device disappears — §9).

"Just put a mutex around the shared buffer" is the instinct, and it's exactly the thing that
turns one slow moment into an audible glitch.

---

## 7. Composition: routing, mixing, and the channel matrix

*After* sources are aligned and on a common timeline, "combining" finally becomes the thing
people pictured: deciding **what goes where**. This is the routing/mixing layer:

- **Mixing** — summing several signals (with per-source gains) into one bus. Watch headroom:
  N full-scale signals summed can clip; you need gain staging or a limiter.
- **Routing / a patch matrix** — input k → bus j, fan-out (one source to many destinations),
  fan-in (many to one).
- **Channel operations** — split a stereo source into two monos, merge, pan a mono into a
  stereo field, up/down-mix. These **change the channel count**, which (see §4) an engine
  must be built to represent.
- **Bus structure** — sub-mixes, sends/returns, monitor mixes distinct from the main mix.

Notably, this — the part that *looks* like the whole problem — is the **easy** part once
alignment (§3–§6) is solved. A clean architecture keeps it that way by making mixing and
routing **ordinary processing nodes in the graph**, not something baked into the I/O layer
(✓ ADR-0008 §4: "mixing/summing is a graph-node responsibility, not the I/O layer's").

---

## 8. Dynamic topology and device lifecycle

Hardware is not static. Mid-stream, a USB interface is unplugged, Bluetooth headphones go to
sleep, the user switches the default output, a device changes its sample rate. A toy demo
crashes; a real pipeline must **survive every one of these without a glitch on the surviving
streams, without a crash, and without blocking the audio thread.**

That requires:
- **Asynchronous notification handling** — the OS reports device changes on a *listener*
  thread, never the audio thread. You listen there.
- **An off-thread reconfiguration state machine** — on a disconnect: stop the dead callback
  cleanly, tear down/rebuild any aggregate device, fall back to a default or surface an
  event, re-open — all coordinated with running start/stop, none of it on the audio thread.
- **Clean teardown of shared resources** — aggregate devices, rings, the clock master.

And it is **hard to test**: reproducing "USB yanked at sample 2,000,000" reliably needs
either physical hardware in the loop or a mocked device layer that can inject "device died"
events. (✓ aiudio's `docs/pipeline/76`/`testing` plan calls out building exactly such a mock backend.)

---

## 9. Failure semantics: graceful degradation

Because you *cannot* guarantee zero xruns (scheduling jitter, a momentarily heavy block, a
slow consumer, a drifting clock), a multi-I/O engine must define **what happens when a buffer
can't be filled or drained in time** — and the behavior must be real-time-safe:

- **Underrun** (no input data ready) → emit **silence** (or a short fade); never play stale
  or garbage data, never block to wait.
- **Overrun** (nowhere to put produced data) → **drop** it (the SPSC ring returns "full"
  rather than overwrite the reader or block).
- **Detect and count** every event into atomic counters exposed as telemetry, so quality
  degradation is visible.
- **Isolate** failures: one slow source must not stall the others — a key reason each source
  is independent (its own ring, its own clock handling).

With multiple sources, the **failure modes multiply**, and the policy has to make each one
independent and recoverable. (✓ this is aiudio's M9.1 "xrun/underrun policy," `docs/pipeline/76`
Phase B.)

---

## 10. Platform divergence

Finally, none of this is uniform across operating systems. Each native audio stack has a
*different model* for multi-device work:

- **macOS Core Audio** — HAL IOProcs per device; **aggregate devices** with drift
  compensation; exclusive/shared modes.
- **ASIO** (Windows pro) — historically **one driver at a time**, which makes true
  multi-device painful and pushes users toward a single multi-channel interface.
- **WASAPI** (Windows) — shared vs exclusive mode, its own resampler in shared mode.
- **JACK** (Linux/pro) — a system-wide graph that *solves* multi-client routing and one
  clock by design, at the cost of requiring the JACK daemon.
- **ALSA / PipeWire** (Linux), **AAudio/OpenSL** (Android), **AudioWorklet** (Web) — each
  with its own callback model, buffer semantics, and multi-device story.

A cross-platform framework must abstract over capabilities that genuinely differ — what's a
built-in aggregate on macOS is a daemon on Linux and "not really supported" on ASIO. The
abstraction (one duplex callback, a swappable clock/backend — ✓ ADR-0005) is what keeps the
engine above this, but each backend pays the platform's price.

---

## 11. How the difficulties compound

The hazards above are not a checklist you clear one by one; they **interact**, and the
interactions are where projects sink:

- Different clocks (§3) force **resampling** (§4), which adds **latency** (§5), which forces
  **delay compensation** (§5) — and the resampler's CPU must still fit the **deadline** (§2).
- **Hot-plug** (§8) of one device in an **aggregate** (§3) can change the master clock,
  forcing every other source's **drift loop** (§3) to re-converge.
- More sources (§3, §6) mean more **rings and threads** (§6), more **failure modes** (§9),
  and more **work per deadline** (§2) — simultaneously.
- A **channel-count change** in routing (§7) requires the engine's buffer model to be
  flexible (§4), which most "fixed stereo" engines are not.

The combinatorics are why "support multiple I/O" is rarely a feature you bolt on; it tends
to be an architecture you commit to.

---

## 12. Taming it: the architectural principles

The good news: decades of audio engineering have converged on a small set of principles that
make multi-I/O tractable. They're worth stating as a checklist, because an engine that honors
them turns the compounding nightmare back into a set of bounded problems. (aiudio adopts all
of these — see the references.)

1. **The audio thread is sacred.** No allocation, locks, syscalls, or interpreter on it.
   Everything risky happens off-thread (✓ ADR-0004).
2. **One clock per source; combine *after* the boundary.** Each device/source is one backend
   on one clock, feeding **one SPSC ring**. Never merge producers into a ring (✓ ADR-0008).
3. **Align before you combine.** Bring off-clock sources onto a single master timeline —
   **aggregate device first, software resampling/drift-comp second** (✓ ADR-0008 §3).
4. **Track and equalize latency.** Every path reports its delay; the engine compensates so
   combined signals stay phase-aligned (PDC).
5. **Mixing and routing are graph nodes, not I/O.** Keep transport/clocking in the I/O layer
   and composition in the processing graph — they evolve independently (✓ ADR-0008 §4).
6. **Make the buffer model flexible** enough to represent different channel counts/rates at
   different points (per-port channel widths; per-edge formats).
7. **Degrade gracefully and observably.** Silence on underrun, drop on overrun, count
   everything, isolate failures per source.
8. **Manage lifecycle off-thread.** A reconfiguration state machine handles hot-plug,
   fallback, and aggregate rebuild without touching the audio thread.
9. **Abstract the platform** behind one callback + swappable backend, and pay each OS's price
   inside the backend (✓ ADR-0005).
10. **A multi-source *manager*** owns the N-backends/N-rings/one-clock coordination as an
    explicit component, rather than scattering it (✓ the `docs/pipeline/76` M10 manager).

Honor those, and the parts that remain hard — drift compensation, latency bookkeeping,
hot-plug recovery — are bounded, well-understood problems with known solutions, instead of an
ever-shifting tangle.

---

## 13. Conclusion

Combining multiple audio inputs and outputs is hard not because mixing is hard — mixing is a
loop with an add — but because **doing it against real hardware means reconciling many
independent physical clocks, on a hard real-time deadline, across many threads, with sources
that disagree on rate, format, channels, and timing, any of which can vanish mid-stream.**
The arithmetic is trivial; the *agreement* — getting every source onto one timeline, in one
format, phase-aligned, glitch-free, and survivable — is the work.

The discipline that makes it possible is to **separate the concerns**: clocking and transport
in a real-time-safe I/O layer (one clock per source, lock-free rings, alignment by aggregate
or resampling), composition in a processing graph (mixing and routing as nodes), and
lifecycle/control off the audio thread. That separation is precisely what lets a framework
present the user with the simple picture they expected — wires in, a mix, wires out — while,
underneath, the hard problems are each solved in their own place.

---

## References
- aiudio decisions: ADR-0004 (RT safety), ADR-0005 (one duplex callback, swappable clock),
  ADR-0008 (multi-input: per-source rings, aggregate/resample, mixing-is-a-node),
  ADR-0009 (graph spine).
- aiudio plans: [`docs/pipeline/76`](76-multi-source-io-roadmap.md) (the multi-source roadmap that
  applies these principles), [`docs/theory/73`](../theory/73-digital-audio-encoding.md) (formats/latency/
  channels primer), [`docs/pipeline/71`](71-io-layer-milestones.md) (the I/O layer).
- Concepts to read further: Core Audio aggregate devices & drift compensation; word clock /
  AES/ADAT sync; polyphase sample-rate conversion (44.1↔48 = 147:160); JACK's graph model;
  plug-in delay compensation (PDC); wait-free SPSC ring buffers.
