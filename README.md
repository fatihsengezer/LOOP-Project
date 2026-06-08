# .LOOP Format — Proprietary Binary Audio Engine

> **For AI Agents & New Contributors:** This document is the single source of
> truth for the project. Read it before touching any source file. Every
> architectural decision that appears "strange" has a documented reason here.

---

## Table of Contents

1. [Project Purpose](#1-project-purpose)
2. [Repository Layout](#2-repository-layout)
3. [Binary Format Specification](#3-binary-format-specification)
4. [Build Instructions](#4-build-instructions)
5. [Implementation Phases & Status](#5-implementation-phases--status)
6. [Core Design Principles](#6-core-design-principles)
7. [Class Reference](#7-class-reference)
8. [Data Flow Diagrams](#8-data-flow-diagrams)
9. [Invariants & Contracts](#9-invariants--contracts)
10. [Known Footguns](#10-known-footguns)
11. [Glossary](#11-glossary)

---

## 1. Project Purpose

A **proprietary `.LOOP` binary audio format** and a dual-purpose application:

| Mode        | Description |
|-------------|-------------|
| **Creator** | A DAW-lite that imports WAV files, arranges them on a timeline grid, and compiles the result to a single `.LOOP` file |
| **Player**  | A zero-latency JUCE `AudioProcessor` that memory-maps the `.LOOP` file and drives a tick-accurate sequencer engine |

**Goals, in priority order:**
1. Sample-accurate timing — no event jitter under any buffer size
2. Minimal RAM usage — large sample banks must not require full preload
3. No heap allocations in the audio thread
4. Human-readable tooling output (the compiler) but machine-optimal runtime format

---

## 2. Repository Layout

```
LOOP_Project/
├── include/
│   ├── FormatSpec.h          ← Binary struct definitions (PACKED). THE CONTRACT.
│   ├── LoopFileWriter.h      ← Creator-side serialiser API
│   ├── LoopFileReader.h      ← Player-side deserialiser + mmap manager
│   ├── ActiveVoice.h         ← Per-voice playback state (lock-free, POD only)
│   ├── LoopSequencer.h       ← juce::AudioSource sequencer engine
│   └── VelocityTable.h       ← Compile-time velocity→gain lookup table
│
├── src/
│   ├── LoopFileWriter.cpp    ← Phase 1a implementation
│   ├── LoopFileReader.cpp    ← Phase 1b implementation
│   ├── LoopSequencer.cpp     ← Phase 2 implementation
│   └── LoopPlayerProcessor.cpp ← Phase 4 JUCE AudioProcessor
│
├── tests/
│   ├── test_format_roundtrip.cpp  ← Write → Read → verify bit-for-bit equality
│   ├── test_sequencer_timing.cpp  ← Verify tick→sample math at several BPMs
│   └── test_zero_crossing.cpp     ← Verify trim logic on synthetic waveforms
│
└── docs/
    ├── file_format_v1.md      ← Byte-map of the binary format (auto-generated)
    └── adr/                   ← Architecture Decision Records
        ├── ADR-001-tick-timing.md
        ├── ADR-002-mmap-strategy.md
        └── ADR-003-bitpacked-events.md
```

---

## 3. Binary Format Specification

### 3.1 File Layout (sequential, no gaps)

```
┌────────────────────────────────────────────────────────────┐
│ FileHeader          64 bytes    offset 0 (always)          │
├────────────────────────────────────────────────────────────┤
│ SampleLUTEntry[0]   32 bytes    ┐                          │
│ SampleLUTEntry[1]   32 bytes    │  FileHeader.lut_offset   │
│ ...                             ┘                          │
├────────────────────────────────────────────────────────────┤
│ SampleDataBlock[0]  16 bytes    ┐                          │
│   PCM payload[0]    N bytes     │  FileHeader.             │
│ SampleDataBlock[1]  16 bytes    │  sample_data_offset      │
│   PCM payload[1]    N bytes     ┘                          │
│ ...                                                        │
├────────────────────────────────────────────────────────────┤
│ SequenceHeader      32 bytes    FileHeader.sequence_offset │
│ SequenceEvent[0]     8 bytes    ┐                          │
│ SequenceEvent[1]     8 bytes    │  sorted by tick_start    │
│ ...                             ┘                          │
├────────────────────────────────────────────────────────────┤
│ MetaChunkHeader     16 bytes    FileHeader.meta_offset     │
│   UTF-8 KV pairs    N bytes     (0 = chunk absent)         │
└────────────────────────────────────────────────────────────┘
```

### 3.2 Magic & Version

- **Magic bytes:** `4C 4F 4F 50` ("LOOP")
- **Version:** `major.minor` — breaking changes → bump major; additive changes → bump minor
- Current: `1.0`

### 3.3 SequenceEvent Bit Layout (64-bit word)

```
 63      71  70  69      47              40          28              0
  └──────┘   │   │  └─────┘          └──┘       └───┘          └───┘
  reserved  muted one_shot  loop_duration(22b)  sample_id(12b)  tick(28b)
                            velocity(7b) @ bits 40–46
```

### 3.4 Endianness

All multi-byte integers are **Little-Endian**. On x86/x64 Windows (the primary
target), this is a no-op. If porting to a BE platform, add a `byteswap.h`
utility and call it in `LoopFileReader` after every multi-byte field read.

---

## 4. Build Instructions

### Prerequisites

| Dependency | Version | Notes |
|------------|---------|-------|
| Visual Studio | 2022 (MSVC 19.3x) | C++20 `/std:c++20` |
| JUCE | ≥ 7.0.5 | Add via Projucer or CMake FetchContent |
| CMake | ≥ 3.24 | Optional; Projucer also works |

### JUCE Projucer Settings

```
C++ Standard:          C++20
Extra Compiler Flags:  /W4 /WX /arch:AVX2
Preprocessor Defines:  LOOP_AUDIO_BLOCK_SIZE=512
                       JUCE_STRICT_REFCOUNTEDPOINTER=1
```

### CMake (alternative)

```cmake
cmake_minimum_required(VERSION 3.24)
project(LOOP VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE via FetchContent or local path
add_subdirectory(JUCE)

juce_add_plugin(LOOPPlayer
    PLUGIN_MANUFACTURER_CODE Loop
    PLUGIN_CODE LpPl
    FORMATS VST3 Standalone
    PRODUCT_NAME "LOOP Player")

target_sources(LOOPPlayer PRIVATE
    src/LoopFileWriter.cpp
    src/LoopFileReader.cpp
    src/LoopSequencer.cpp
    src/LoopPlayerProcessor.cpp)

target_include_directories(LOOPPlayer PRIVATE include)
target_compile_options(LOOPPlayer PRIVATE /W4 /WX /arch:AVX2)
```

---

## 5. Implementation Phases & Status

| Phase | Description | Status | Entry Point |
|-------|-------------|--------|-------------|
| 1a | `LoopFileWriter` — serialise WAVs + timeline to `.LOOP` | ✅ Done | `LoopFileWriter.h` |
| 1b | `LoopFileReader` — mmap-based reader + LUT cache | ✅ Done | `LoopFileReader.h` |
| 2  | `LoopSequencer` — tick-accurate AudioSource | 🔲 Next | `LoopSequencer.h` |
| 3  | `TimelineComponent` — DAW-lite GUI | 🔲 Pending | — |
| 4  | `LoopPlayerProcessor` — JUCE AudioProcessor shell | 🔲 Pending | — |

---

## 6. Core Design Principles

### P1 — Tick-Based Timing (TPQN = 960)

We **never** store event timestamps in milliseconds. Reason: if BPM changes
from 120 → 140, every millisecond-based timestamp would drift off the grid.
Tick-based timing keeps events mathematically "on the grid" at any BPM.

```
sample_position = tick × (sample_rate × 60) / (bpm × TPQN)
```

See `FormatSpec.h :: tickToSample()` and `ADR-001`.

### P2 — Zero-Crossing Trim

When the Creator Tool saves a loop, it trims the end to the nearest zero-
crossing (where the waveform amplitude is near 0). This prevents a "click"
when the Player jumps back to frame 0 on loop repeat.

See `FormatSpec.h :: findZeroCrossing_16()`.

### P3 — `#pragma pack(push, 1)` on All Disk Structs

Every struct that maps directly to file bytes is wrapped in
`LOOP_PACKED_BEGIN / LOOP_PACKED_END`. This guarantees `sizeof()` equals the
exact byte count we expect. Forgetting this → silent data corruption on
round-trips because the compiler inserts invisible padding.

**Important:** Never do arithmetic-intensive work directly on packed struct
members from a mapped region. Copy to a local aligned variable first if the
compiler or CPU warns about unaligned access.

### P4 — Zero Heap Allocation in the Audio Thread

All heap allocation happens in `prepareToPlay()`. The `processBlock()` loop
operates exclusively on:
- Stack-allocated local variables
- Pre-allocated `std::array` voice pool
- `juce::AbstractFifo` ring (lock-free)
- Read-only pointers into the memory-mapped file

### P5 — Memory-Mapped Sample Access

`LoopFileReader` opens the file once with `juce::MemoryMappedFile`. Each
`ActiveVoice` holds a raw pointer into the mapped region. The OS page-fault
mechanism loads only the 4K pages actually being read — not the whole file.
A 2GB sample bank with 4 playing voices uses ~4 pages (~16KB) of RAM.

---

## 7. Class Reference

### `LoopFileWriter`

| Method | Thread | Description |
|--------|--------|-------------|
| `addSample(buffer, meta)` | Any | Converts float PCM, trims to ZC, enqueues |
| `addEvent(event)` | Any | Enqueues a SequenceEvent |
| `write(path)` | Any | Serialises everything to disk atomically |
| `clear()` | Any | Resets internal state; safe to reuse |

### `LoopFileReader`

| Method | Thread | Description |
|--------|--------|-------------|
| `open(path)` | Non-audio | Memory-maps file, validates, builds LUT |
| `isValid()` | Any | Returns true if file is open and CRC-verified |
| `getLUTEntry(id)` | Any | O(1) LUT lookup by sample ID |
| `getSamplePointer(id)` | Audio-safe | Raw pointer into mapped PCM |
| `getSequenceHeader()` | Any | Returns const ref to in-memory header |
| `getEvent(index)` | Audio-safe | Returns const ref to event in mmap'd array |

### `LoopSequencer` _(Phase 2)_

Inherits `juce::AudioSource`. Drives the voice pool from the event array.
See `LoopSequencer.h` when Phase 2 is implemented.

---

## 8. Data Flow Diagrams

### Creator Tool (Write Path)

```
WAV File(s)
    │
    ▼
LoopFileWriter::addSample()
    ├─► float→int16/24 conversion
    ├─► Zero-crossing trim
    ├─► CRC32 of raw PCM
    └─► Stored in internal SampleBank vector
         │
Timeline Grid
    │
    ▼
LoopFileWriter::addEvent()
    └─► Stored in internal EventList vector
         │
         ▼
LoopFileWriter::write(path)
    ├─► Open FileOutputStream
    ├─► Write FileHeader (offsets = 0, patched later)
    ├─► Write SampleLUT array
    ├─► For each sample: write SampleDataBlock header + PCM bytes
    ├─► Write SequenceHeader + sorted SequenceEvent array
    ├─► Write MetaChunk
    ├─► Seek back → patch all offsets in FileHeader
    └─► Compute + write FileHeader CRC32
```

### Player (Read Path)

```
.LOOP File on disk
    │
    ▼
LoopFileReader::open()
    ├─► juce::MemoryMappedFile (whole file, READ_ONLY)
    ├─► Validate magic + version + header CRC32
    ├─► Build std::array<SampleLUTEntry, kMaxSamples> index
    └─► isValid_ = true
         │
         ▼                         (audio thread, every processBlock)
LoopSequencer::getNextAudioBlock()
    ├─► Advance playhead by blockSize samples
    ├─► Convert playhead to ticks
    ├─► Binary-search event array for events in look-ahead window
    ├─► For each triggered event:
    │       voice.pcmPtr  = reader.getSamplePointer(evt.sampleID())
    │       voice.gainLin = velocityTable[evt.velocity()]
    │       push → AbstractFifo voice ring
    └─► Mix all active voices → output buffer
```

---

## 9. Invariants & Contracts

These must hold at all times. Tests verify each one.

1. `FileHeader.magic == {'L','O','O','P'}` — violated → abort load
2. `FileHeader.version_major == 1` — violated → return `Error::IncompatibleVersion`
3. `SequenceEvent[]` is **sorted ascending by `tickStart`** — required for
   binary-search in the sequencer. Enforced by `LoopFileWriter::write()`.
4. `SampleLUTEntry.file_offset` points to a `SampleDataBlock` whose
   `sample_id` matches the LUT entry's `sample_id` — verified on open.
5. `SampleLUTEntry.frame_count` ≤ actual PCM byte_length / bytesPerFrame()
6. No `SequenceEvent` references a `sample_id` ≥ `FileHeader.sample_count`

---

## 10. Known Footguns

| # | Footgun | Mitigation |
|---|---------|------------|
| 1 | Mixing packed-struct pointers with unaligned reads on ARM | Copy LUT entries to a local `SampleLUTEntry local = *ptr` before any field access |
| 2 | `juce::MemoryMappedFile` returns `nullptr` on map failure | `LoopFileReader::open()` checks this; always check `isValid()` before calling `getSamplePointer()` |
| 3 | Event array not sorted → binary search returns wrong events | `write()` calls `std::sort` on events before serialising; do not write events manually |
| 4 | 24-bit PCM: bytes are packed (3 bytes/sample, not 4) | `SampleDataBlock::byte_length` is the ground truth; never compute it from `frame_count * 3` without checking `channels` |
| 5 | BPM change mid-sequence doesn't recompute stored tick values | Ticks are BPM-independent (that's the point). Recompute `tickToSample()` at runtime with the new BPM |
| 6 | `crc32_header` covers bytes [0..51] only, not itself | `LoopFileWriter` zeroes bytes [52..55] before computing, then writes the result there. `LoopFileReader` must do the same zero-then-verify dance |

---

## 11. Glossary

| Term | Definition |
|------|------------|
| **Tick** | The atomic time unit. TPQN ticks = 1 quarter note |
| **TPQN** | Ticks Per Quarter Note. Default 960. Never changes within a file |
| **Frame** | One sample across all channels (e.g. L+R = 1 frame at 2ch) |
| **LUT** | Look-Up Table — the `SampleLUTEntry[]` array for O(1) ID→offset |
| **mmap** | Memory-mapped file. The OS maps file pages into the process address space on demand |
| **Look-ahead** | The N-tick window ahead of the current playhead that the sequencer scans for upcoming events |
| **ZC / Zero-Crossing** | A sample frame where all channel amplitudes are near 0. Used as a safe loop boundary |
| **ActiveVoice** | A currently-playing loop instance. Up to 64 may be active simultaneously |
| **One-Shot** | A SequenceEvent flag meaning "play to the natural end; do not loop" |
| **PCM** | Pulse-Code Modulation — raw, uncompressed audio samples |
