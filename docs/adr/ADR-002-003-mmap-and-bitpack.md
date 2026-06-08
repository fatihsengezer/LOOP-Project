# ADR-002 — Memory-Mapped Sample Access

**Status:** Accepted  
**Date:** 2025

---

## Context

Sample banks may be hundreds of megabytes. The player must begin playing
immediately without a multi-second preload, and must not exhaust physical RAM
on machines with large libraries.

Candidates:
1. **Full preload** — `std::vector<uint8_t>` populated by `fread()` at open time
2. **On-demand streaming** — background thread prefetches ahead of playhead
3. **Memory-mapping** — OS manages paging transparently

---

## Decision

Use `juce::MemoryMappedFile` (wraps `mmap()` on POSIX / `MapViewOfFile` on Win32)
to map the entire file READ-ONLY. Each `ActiveVoice` holds a raw `const uint8_t*`
pointer into the mapped region.

---

## Consequences

### Positive

- **Instant open.** `open()` returns in < 1ms regardless of file size. No bytes
  are physically read until a pointer into the map is dereferenced.
- **Proportional RAM usage.** Only pages (4KB each) that are actively being
  read exist in physical RAM. 10 inactive samples in a 2GB bank = 0 extra RAM.
- **Zero-copy.** The audio thread reads PCM bytes directly from OS-managed
  virtual memory. No `memcpy()` from a "preload buffer" into an "output buffer."
- **Multi-instance sharing.** Multiple plugin instances mapping the same file
  share physical pages (copy-on-write semantics). Important for multi-track hosts.
- **OS prefetching.** On sequential playback, the OS's readahead heuristics
  pre-fetch pages before the audio thread needs them, preventing page-fault
  stalls in `processBlock()`.

### Negative / Mitigations

- **Page-fault on first access.** If a sample hasn't been played since the file
  was opened, the first `processBlock()` that reads it may trigger a page fault.
  **Mitigation:** The sequencer's look-ahead window (2 ticks ≈ 1ms) is large
  enough to absorb a single page fault at typical disk speeds. On SSDs (< 0.1ms
  random read), this is effectively zero latency.
- **File must remain on disk.** The mapping is invalidated if the file is
  deleted or moved while mapped. **Mitigation:** `LoopFileReader::open()` opens
  with `exclusiveAccessMode=false` on Windows, which prevents deletion while
  mapped. On macOS/Linux, the inode persists until the last reference is released.
- **Must call `close()` before file can be overwritten.** The Creator Tool must
  call `reader.close()` before re-compiling to the same path.

---

---

# ADR-003 — Bit-Packed SequenceEvent (64-bit word)

**Status:** Accepted  
**Date:** 2025

---

## Context

The sequencer must scan thousands of events per second. Event data needs to be:
1. Small (fits in L1/L2 cache during binary search)
2. Fast to decode (single instruction to extract a field)
3. Large enough to hold musically meaningful ranges

---

## Decision

Each `SequenceEvent` is a single `uint64_t` with fields packed at the bit level:

```
Bits  0–27 : tick_start      (28 bits) — max 268M ticks ≈ 38 hours at 120BPM
Bits 28–39 : sample_id       (12 bits) — 0..4095 samples
Bits 40–46 : velocity        ( 7 bits) — 0..127 (MIDI-standard)
Bits 47–68 : loop_duration   (22 bits) — 0..4M ticks ≈ 41 minutes
Bit  69    : flag_one_shot   ( 1 bit)
Bit  70    : flag_muted      ( 1 bit)
Bits 71–63 : _reserved       (remaining)
```

Extraction uses masks and shifts only — no struct member accesses or branches.

---

## Consequences

### Positive

- **Size.** 8 bytes per event. 1 million events = 8MB, fitting comfortably in
  L2 cache on any modern CPU.
- **Binary search.** `std::lower_bound` on a sorted array of `uint64_t` is
  highly cache-friendly — no pointer chasing, sequential memory layout.
- **Atomic reads.** A 64-bit read on a 64-bit CPU is a single `MOV` instruction
  — no torn reads, no partial-word issues.
- **Future extensibility.** Bits 71–63 are reserved. Adding new flags
  (pitch, pan, etc.) is a v1.1 change with no file format break.

### Negative / Mitigations

- **Readability.** Bit manipulation code is harder to read than named struct
  fields. **Mitigation:** All bit operations are in `SequenceEvent` accessor/
  mutator methods. Users never write `event.data & 0xFFF...` directly.
- **Max sample count = 4096.** The 12-bit `sample_id` field caps at 4095.
  This is far more than any practical sample bank. If exceeded, `addEvent()`
  asserts in debug and silently truncates in release — add a validation step
  in `LoopFileWriter` if you approach this limit.
- **No per-event BPM override.** All events share the master BPM. If you need
  polyrhythmic BPM changes, this format will need a TempoMap chunk (v2.0 scope).

---

## Rejected Alternative: Struct of Named Fields

A `struct { uint32_t tick; uint16_t id; uint8_t vel; ... }` with `#pragma pack`
would be ~10 bytes minimum (plus alignment). More readable, but:
- 25% larger → worse cache utilisation during seek
- Less atomic-friendly on 32-bit platforms
- No meaningful benefit: the accessor methods provide the readability layer
