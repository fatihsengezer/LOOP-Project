# ADR-001 — Tick-Based Timing (TPQN = 960)

**Status:** Accepted  
**Date:** 2025  
**Deciders:** Engine Architecture Team

---

## Context

When scheduling audio events (loop triggers, note-ons, etc.), we need a time
representation that is:

1. BPM-agnostic in storage — changing the tempo at playback time should not
   require rewriting any stored events
2. Musically quantised — events should snap to beats, bars, and subdivisions
3. Precise enough for sample-accurate playback at high sample rates

The two main candidates were:
- **Millisecond-based** timestamps (store `float ms_offset`)
- **Tick-based** timestamps (store `uint32_t tick`, where N ticks = 1 quarter note)

---

## Decision

We use **TPQN-based ticks** (Ticks Per Quarter Note), with a default of **960
ticks per quarter note** (same as Pro Tools, Logic Pro, and Ableton Live).

The runtime conversion formula is:

```
sample_position = tick × (sample_rate × 60) / (bpm × TPQN)
```

This is evaluated in `FormatSpec.h::tickToSample()` on every block.

---

## Consequences

### Positive

- **BPM changes are free.** If the user changes BPM from 120 to 140, no event
  data is modified. The sequencer recomputes `tickToSample()` with the new BPM
  and all events land on the correct sample automatically.

- **Grid-perfect quantisation.** A 1-bar event at 4/4 is always exactly
  `4 × TPQN = 3840` ticks, regardless of BPM or sample rate.

- **DAW interoperability.** TPQN-960 is the standard. If we ever export
  to MIDI, ticks map directly.

- **Integer storage.** A 28-bit tick field in `SequenceEvent` supports
  ~268 million ticks — at 960 TPQN / 120 BPM, that is over 38 hours of music.

### Negative

- **Runtime division.** Every tick-to-sample conversion requires a floating-
  point divide. Mitigated by pre-computing `samplesPerTick` once per block
  and multiplying in the scheduler (no per-event division).

- **Sub-tick precision is lost.** At 44100 Hz / 120 BPM / TPQN=960,
  one tick ≈ 22.97 samples. We round to the nearest integer sample.
  This ≈ ±11 sample jitter is inaudible (~0.25ms). Sub-tick precision
  would require floating-point tick storage, which costs 32 bits vs 28
  and complicates the bit-packed `SequenceEvent`.

---

## Rejected Alternative: Millisecond Timestamps

Milliseconds would require re-timestamping every event on BPM change. For a
live performance tool where BPM is adjustable in real-time, this is
unacceptable. Additionally, a millisecond-domain event grid does not snap
cleanly to musical subdivisions (1 beat at 120BPM = 500ms; at 121BPM ≈
495.87ms — not an integer).
