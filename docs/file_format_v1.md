# .LOOP Binary Format — Byte Map (v1.0)

> Auto-generated reference document. Do **not** edit struct sizes manually —  
> change `FormatSpec.h` and re-run the static_assert checks.

---

## File Layout (sequential, no gaps)

```
Offset 0
├─ FileHeader           (64 bytes)
│
├─ SampleLUT
│   ├─ SampleLUTEntry[0]   (32 bytes)
│   ├─ SampleLUTEntry[1]   (32 bytes)
│   └─ ...  × sample_count
│
├─ SampleData
│   ├─ SampleDataBlock[0]  (16 bytes header)
│   │   └─ PCM payload[0]  (byte_length bytes)
│   ├─ SampleDataBlock[1]  (16 bytes header)
│   │   └─ PCM payload[1]
│   └─ ...  × sample_count
│
├─ SequenceChunk
│   ├─ SequenceHeader       (32 bytes)
│   └─ SequenceEvent[0..N-1]  (8 bytes each)
│
└─ MetaChunk  (optional — absent if meta_offset == 0)
    ├─ MetaChunkHeader      (16 bytes)
    └─ UTF-8 "key=value\n" payload  (meta_byte_length bytes)
```

---

## 1. FileHeader — 64 bytes @ offset 0

| Offset | Size | Field               | Type      | Notes |
|--------|------|---------------------|-----------|-------|
| +0     | 4    | `magic`             | uint8[4]  | `{'L','O','O','P'}` — ASCII, not null-terminated |
| +4     | 2    | `version_major`     | uint16 LE | Currently `1`. Breaking changes bump this. |
| +6     | 2    | `version_minor`     | uint16 LE | Currently `0`. Additive changes bump this. |
| +8     | 4    | `sample_count`      | uint32 LE | Number of SampleLUTEntry / SampleDataBlock pairs |
| +12    | 4    | `event_count`       | uint32 LE | Number of SequenceEvent entries |
| +16    | 8    | `lut_offset`        | uint64 LE | Absolute file offset → first SampleLUTEntry |
| +24    | 8    | `sample_data_offset`| uint64 LE | Absolute file offset → first SampleDataBlock |
| +32    | 8    | `sequence_offset`   | uint64 LE | Absolute file offset → SequenceHeader |
| +40    | 8    | `meta_offset`       | uint64 LE | Absolute file offset → MetaChunkHeader; **0 = absent** |
| +48    | 2    | `tpqn`              | uint16 LE | Ticks Per Quarter Note (default 960) |
| +50    | 2    | `_reserved0`        | uint16    | Must be 0 |
| +52    | 4    | `crc32_header`      | uint32 LE | CRC-32 of bytes [0..51]; self-excluded (zeroed before computing) |
| +56    | 8    | `_padding`          | uint8[8]  | Reserved, must be 0 |

**Total: 64 bytes.** Verified by `static_assert(sizeof(FileHeader) == 64)`.

### CRC-32 Computation Rule

```
header.crc32_header = 0;           // zero the field
header.crc32_header = crc32(&header, 52);  // compute over bytes [0..51]
```

Verifier (reader) must apply the same zero-then-compute dance.

---

## 2. SampleLUTEntry — 32 bytes each

Begins at `FileHeader.lut_offset`. There are `FileHeader.sample_count` entries,
stored contiguously. Indexed by `sample_id` (== array index).

| Offset | Size | Field         | Type      | Notes |
|--------|------|---------------|-----------|-------|
| +0     | 2    | `sample_id`   | uint16 LE | Matches index in the array (0-based) |
| +2     | 1    | `channels`    | uint8     | `1` = mono, `2` = stereo |
| +3     | 1    | `flags`       | uint8     | Bit flags (see below) |
| +4     | 4    | `sample_rate` | uint32 LE | e.g. 44100 or 48000 Hz |
| +8     | 4    | `frame_count` | uint32 LE | Total PCM frames (post zero-crossing trim) |
| +12    | 4    | `byte_length` | uint32 LE | Raw PCM byte count (`frame_count × bytesPerFrame`) |
| +16    | 8    | `file_offset` | uint64 LE | Absolute offset → corresponding SampleDataBlock |
| +24    | 4    | `base_note`   | float32   | MIDI root note (e.g. 60.0 = C4); `0.0` if unset |
| +28    | 4    | `loop_bpm`    | float32   | Original BPM; `0.0` = free (non-tempo-sync'd) |

**Total: 32 bytes.** Verified by `static_assert(sizeof(SampleLUTEntry) == 32)`.

### Flags Byte

| Bit mask | Constant        | Meaning |
|----------|-----------------|---------|
| `0x01`   | `kBitDepth16`   | PCM is 16-bit signed integer |
| `0x02`   | `kBitDepth24`   | PCM is 24-bit signed integer (packed 3 bytes/sample) |
| `0x04`   | `kFlagStereo`   | 2 channels; channels are interleaved |
| `0x08`   | `kFlagLoopable` | End trimmed to nearest zero-crossing |

Exactly one of `kBitDepth16` / `kBitDepth24` must be set.

### bytesPerFrame Derivation

```
bytesPerFrame = channels × (bitDepth / 8)
```

- Mono 16-bit:   2 bytes/frame
- Stereo 16-bit: 4 bytes/frame
- Mono 24-bit:   3 bytes/frame
- Stereo 24-bit: 6 bytes/frame

---

## 3. SampleDataBlock — 16 bytes header + PCM payload

Begins at `FileHeader.sample_data_offset`. Each entry starts with a 16-byte
header followed immediately by `byte_length` bytes of raw PCM.

| Offset | Size | Field         | Type      | Notes |
|--------|------|---------------|-----------|-------|
| +0     | 4    | `tag`         | uint32 LE | FourCC `0x44415053` ("SPAD") for sanity |
| +4     | 2    | `sample_id`   | uint16 LE | Must match the corresponding LUT entry |
| +6     | 2    | `_reserved`   | uint8[2]  | Must be 0 |
| +8     | 4    | `byte_length` | uint32 LE | PCM payload byte count (follows this header) |
| +12    | 4    | `crc32_pcm`   | uint32 LE | CRC-32 over the raw PCM bytes |

**Total header: 16 bytes.** Followed immediately by `byte_length` PCM bytes.

### PCM Layout (interleaved, Little-Endian)

**16-bit stereo example** (3 frames, L+R interleaved):
```
[L0_lo][L0_hi][R0_lo][R0_hi] [L1_lo][L1_hi][R1_lo][R1_hi] [L2_lo]...
```

**24-bit mono example** (3 frames, 3 bytes/sample LE):
```
[S0_b0][S0_b1][S0_b2] [S1_b0][S1_b1][S1_b2] [S2_b0]...
```

Sign-extension for 24-bit: if bit 23 is set, the upper byte is `0xFF`.

---

## 4. SequenceHeader — 32 bytes

Located at `FileHeader.sequence_offset`.

| Offset | Size | Field                   | Type      | Notes |
|--------|------|-------------------------|-----------|-------|
| +0     | 4    | `tag`                   | uint32 LE | FourCC `0x51455353` ("SSEQ") |
| +4     | 4    | `event_count`           | uint32 LE | Number of SequenceEvents that follow |
| +8     | 4    | `total_duration_ticks`  | uint32 LE | Full sequence length in ticks (for loop wrap) |
| +12    | 2    | `time_sig_numerator`    | uint16 LE | e.g. `4` (for 4/4) |
| +14    | 1    | `time_sig_denominator`  | uint8     | e.g. `4` (must be power of 2) |
| +15    | 1    | `_reserved0`            | uint8     | Must be 0 |
| +16    | 4    | `bpm`                   | float32   | Master tempo |
| +20    | 4    | `crc32_events`          | uint32 LE | CRC-32 over all SequenceEvent bytes |
| +24    | 8    | `_padding`              | uint8[8]  | Reserved, must be 0 |

**Total: 32 bytes.** Followed immediately by `event_count × 8` bytes of events.

---

## 5. SequenceEvent — 8 bytes (one 64-bit word, bit-packed)

The array immediately follows the SequenceHeader. **Sorted ascending by `tick_start`** — required for binary-search in the player.

```
 Bit 63..71  70  69      47              40          28              0
  └──────┘   │   │  └─────┘          └──┘       └───┘          └───┘
  reserved  muted one_shot  loop_duration(22b)  sample_id(12b)  tick(28b)
                            velocity(7b) @ bits 40–46
```

| Bits  | Width | Field           | Notes |
|-------|-------|-----------------|-------|
| 0–27  | 28    | `tick_start`    | Trigger position in ticks |
| 28–39 | 12    | `sample_id`     | Index into the LUT (0..4095) |
| 40–46 | 7     | `velocity`      | MIDI velocity 0..127 |
| 47–68 | 22    | `loop_duration` | Duration in ticks (0 for one-shot — see `one_shot` flag) |
| 69    | 1     | `one_shot`      | If set: play to end of sample, ignore `loop_duration` |
| 70    | 1     | `muted`         | If set: event is silenced (not triggered) |
| 71–63 | —     | `_reserved`     | Must be 0 |

---

## 6. MetaChunk — optional

Located at `FileHeader.meta_offset`. Absent if `meta_offset == 0`.

### MetaChunkHeader — 16 bytes

| Offset | Size | Field              | Type      | Notes |
|--------|------|--------------------|-----------|-------|
| +0     | 4    | `tag`              | uint32 LE | FourCC `0x4154454D` ("META") |
| +4     | 4    | `meta_byte_length` | uint32 LE | Byte count of UTF-8 payload |
| +8     | 4    | `entry_count`      | uint32 LE | Number of key=value pairs |
| +12    | 4    | `_reserved`        | uint32    | Must be 0 |

**Total header: 16 bytes.** Followed by `meta_byte_length` bytes of UTF-8.

### MetaChunk Payload Format

```
key=value\n
key2=value2\n
...
```

- One entry per line, terminated by `\n` (0x0A)
- Keys must not contain `=` or `\n`
- Values must not contain `\n`
- Encoding: UTF-8

### Reserved MetaChunk Keys

| Key           | Meaning |
|---------------|---------|
| `sN.name`     | Human name for sample N (e.g. `s0.name=kick`) |
| `project`     | Project/pack name |
| `author`      | Creator name |
| `created`     | ISO-8601 creation timestamp |

---

## 7. Endianness & Alignment

- All multi-byte integers: **Little-Endian**
- All structs use `#pragma pack(push, 1)` — **no compiler padding**
- Audio thread reads from `lutCache_` (aligned copy), never directly from mmap

---

## 8. Version Compatibility Matrix

| File major | File minor | Reader (1.0) | Notes |
|------------|------------|--------------|-------|
| 1          | 0          | ✅ Full       | Current |
| 1          | > 0        | ✅ Partial    | Unknown minor fields ignored |
| ≠ 1        | any        | ❌ Reject     | `IncompatibleVersion` error |

---

## 9. CRC-32 Algorithm

Uses ISO 3309 / ITU-T V.42 polynomial `0xEDB88320` (reflected `0x04C11DB7`).

```
uint32_t crc32(const void* data, size_t len, uint32_t crc = 0xFFFFFFFF)
{
    // 16-entry nibble table for poly 0xEDB88320
    // Final XOR with 0xFFFFFFFF
}
```

Implementation in `FormatSpec.h :: crc32()`.
