# ADR-0018: Trainable-filter form — reparameterized design params + magnitude-response training

- **Status:** Accepted
- **Date:** 2026-07-01
- **Deciders:** Project owner + Claude Code
- **Related:** ADR-0016 (differentiable execution), [`docs/79`](../docs/79-phase1-differentiable-core-roadmap.md) (D2),
  [`docs/20`](../docs/20-differentiable-dsp-and-neural-audio.md) §2.2, `docs/82` (biquad params)

## Context

Phase 1 needs **trainable filters**. But (✓ `docs/20` §2.2) the **recursive structure of IIR /
all-pole filters impedes end-to-end autodiff**, and training a direct-form biquad's **raw
coefficients** is ill-conditioned (the coefficient→response map is non-convex and unstable; small
coefficient nudges can even push poles outside the unit circle). aiudio's RT filter is the
`BiquadNode` (RBJ-cookbook direct form). The differentiable layer must be able to (a) optimize a
filter against an audio/response objective and (b) hand the result back to that `BiquadNode` for
real time (the D6 round-trip).

## Decision

**We will train filters in *design-parameter* space through their *analytic magnitude response*,
not in raw-coefficient space through the time-domain recursion.** Concretely (`aiudio.diff.DiffBiquad`):

- **Parameterize** by the RBJ design params — **cutoff, Q, gain_dB** — and **reparameterize** them
  for well-conditioned optimization: cutoff in **log-frequency**, Q via **softplus** (keeps Q > 0),
  gain in dB (unconstrained).
- **Compute coefficients with the exact same RBJ formulas as C++ `BiquadNode::design()`**, so a
  trained `DiffBiquad` **exports** to a C++ `BiquadNode` (`Graph.add_biquad_coeffs`) faithfully —
  verified by a formula-parity + export-round-trip test against the C++ impulse response.
- **Train through the differentiable analytic response** ``|H(e^jω)|`` (a closed-form, stable
  function of the design params) — the "frequency-sampling" workaround for the IIR-autodiff
  problem. Running a filter *inside* a differentiable graph forward (time-domain recursion) is a
  separate concern handled with the recursive nodes (scan / truncated BPTT) in **D3**.

## Consequences

**Positive**
- Well-conditioned, stable filter training (log-freq + softplus-Q), sidestepping the direct-form
  IIR-autodiff instability (`docs/20` §2.2).
- Exact bridge to RT: the trained filter *is* a `BiquadNode` (same coefficient math) — closes the
  D6 round-trip for filters.
- Interpretable params (cutoff/Q/gain) — agent- and human-legible (Phase 2).

**Negative / costs**
- Two filter representations to keep aligned (torch `DiffBiquad` vs C++ `BiquadNode`) — mitigated
  by the shared formula + the parity/export tests.
- Magnitude-only training discards phase; fine for EQ/timbre matching, a limitation for
  phase-sensitive tasks (revisit if needed).

**Neutral / follow-ups**
- The **time-domain** recursive filter node (biquad/SVF processing audio inside the graph) lands
  in D3 with the other recursive nodes (DcBlocker, Delay) via scan/BPTT.
- A true state-variable-filter *topology* (vs the RBJ biquad used here) can be added later if a
  filter needs to be modulated at audio rate; the design-param + magnitude-response approach is the
  trainable *form*, independent of the RT topology.

## Alternatives considered

- **Train raw direct-form coefficients** — rejected: ill-conditioned, can go unstable (`docs/20` §2.2).
- **Time-domain recursive training (BPTT through `lfilter`)** — viable but costlier/less stable for
  a pure response-match; deferred to D3 where recursion is the theme (needed for filters *inside* a
  graph forward, not for response matching).
- **SVF state-variable topology as the trainable object** — the same well-conditioning is obtained
  more simply by training the RBJ design params via the analytic response; SVF topology can be
  added later for audio-rate modulation.

## References
- [`docs/20`](../docs/20-differentiable-dsp-and-neural-audio.md) §2.2 (IIR autodiff is hard),
  [`docs/79`](../docs/79-phase1-differentiable-core-roadmap.md) §4/§5 (D2), `docs/82` (biquad design params).
- ADR-0016 (differentiable execution + parity harness).
