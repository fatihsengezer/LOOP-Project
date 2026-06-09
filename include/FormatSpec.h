/**
 * =============================================================================
 * FormatSpec.h  —  Proprietary .LOOP Binary Audio Format  (v1.0)
 * =============================================================================
 *
 * Architecture Overview
 * ---------------------
 *  [FileHeader]          – 64 bytes, always at offset 0
 *  [SampleLUT[n]]        – n × SampleLUTEntry (variable, starts at lut_offset)
 *  [SampleChunk[n]]      – raw PCM blocks   (starts at sample_data_offset)
 *  [SequenceChunk]       – SequenceHeader + SequenceEvent[m]
 *                          (starts at sequence_offset)
 *  [MetaChunk]           – optional UTF-8 key=value pairs
 *                          (starts at meta_offset, 0 = absent)
 *
 * Endianness   : Little-Endian (all multi-byte integers)
 * Alignment    : All structs are packed (no compiler padding)
 * PCM encoding : Signed integer, interleaved, 16-bit OR 24-bit
 * Timing       : Tick-based (TPQN = 960 by default)
 *
 * Engineer Notes
 * --------------
 *  1. TPQN-based timing  – BPM changes never disturb the grid.
 *     Tick → Sample:  sample_pos = tick × (sample_rate × 60) / (bpm × TPQN)
 *
 *  2. Zero-crossing trim – Every loop's end_sample is snapped to the nearest
 *     zero-crossing before serialisation (see ZeroCrossingFinder helper).
 *     Prevents click artefacts on loop boundaries.
 *
 *  3. #pragma pack(push,1) – Eliminates all compiler padding so
 *     sizeof(FileHeader) == 64, sizeof(SequenceEvent) == 8, etc.
 *     Safe for disk I/O; always copy to an aligned local struct before
 *     intensive arithmetic if the CPU penalises unaligned access.
 *
 *  4. Memory-mapped access – SampleChunk is designed for mmap/
 *     juce::MemoryMappedFile.  Each entry stores its absolute file offset
 *     so the audio thread can seek directly without parsing.
 *
 *  5. Lock-free audio thread – SequenceEvent array is read-only after load.
 *     The sequencer engine uses a single atomic "playhead tick" and a small
 *     look-ahead ring buffer; zero heap allocation in processBlock().
 * =============================================================================
 */

#pragma once

#include <cstdint>
#include <cstring>      // memcmp
#include <cassert>
#include <array>

// ─────────────────────────────────────────────────────────────────────────────
//  Compiler-portability shims
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_MSC_VER)
    #define LOOP_PACKED_BEGIN   __pragma(pack(push, 1))
    #define LOOP_PACKED_END     __pragma(pack(pop))
    #define LOOP_FORCE_INLINE   __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define LOOP_PACKED_BEGIN   _Pragma("pack(push, 1)")
    #define LOOP_PACKED_END     _Pragma("pack(pop)")
    #define LOOP_FORCE_INLINE   __attribute__((always_inline)) inline
#else
    #define LOOP_PACKED_BEGIN
    #define LOOP_PACKED_END
    #define LOOP_FORCE_INLINE   inline
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Global Constants
// ─────────────────────────────────────────────────────────────────────────────
namespace LoopFormat
{
    // Magic bytes: ASCII "LOOP" + sentinel
    constexpr std::array<uint8_t, 4> kMagic       = { 'L', 'O', 'O', 'P' };
    constexpr uint16_t               kVersionMajor = 1;
    constexpr uint16_t               kVersionMinor = 0;

    // Timing
    constexpr uint16_t kDefaultTPQN = 960;   ///< Ticks Per Quarter Note

    // Limits (can be raised; stored values are 16-bit IDs)
    constexpr uint16_t kMaxSamples   = 4096;
    constexpr uint32_t kMaxEvents    = 1u << 20;  // ~1 million events

    // Bit-depth flags stored in SampleLUTEntry::flags
    constexpr uint8_t kBitDepth16    = 0x01;
    constexpr uint8_t kBitDepth24    = 0x02;
    constexpr uint8_t kFlagStereo    = 0x04;  ///< 0 = mono, 1 = stereo
    constexpr uint8_t kFlagLoopable  = 0x08;  ///< end trimmed to zero-crossing

    // Chunk tags (4-byte FourCC, LE)
    constexpr uint32_t kTagSampleLUT  = 0x544C4153; // "SALT" – Sample LookUp Table
    constexpr uint32_t kTagSampleData = 0x44415053; // "SPAD" – Sample PAD data
    constexpr uint32_t kTagSequence   = 0x51455353; // "SSEQ" – Sequence
    constexpr uint32_t kTagMeta       = 0x4154454D; // "META"

} // namespace LoopFormat

// ─────────────────────────────────────────────────────────────────────────────
//  Packed structure definitions
// ─────────────────────────────────────────────────────────────────────────────

LOOP_PACKED_BEGIN

namespace LoopFormat
{

// ┌─────────────────────────────────────────────────────────────────────────┐
// │  FileHeader   (64 bytes exactly)                                        │
// │  Always resides at file offset 0.                                       │
// └─────────────────────────────────────────────────────────────────────────┘
struct FileHeader
{
    uint8_t  magic[4];          ///< {'L','O','O','P'}                   +0
    uint16_t version_major;     ///< Breaking changes increment this     +4
    uint16_t version_minor;     ///< Backwards-compatible additions      +6

    uint32_t sample_count;      ///< Number of samples in the LUT        +8
    uint32_t event_count;       ///< Number of SequenceEvents            +12

    uint64_t lut_offset;        ///< File offset → SampleLUT array       +16
    uint64_t sample_data_offset;///< File offset → first SampleDataBlock +24
    uint64_t sequence_offset;   ///< File offset → SequenceHeader        +32
    uint64_t meta_offset;       ///< File offset → MetaChunk (0=absent)  +40

    uint16_t tpqn;              ///< Ticks Per Quarter Note (e.g. 960)   +48
    uint16_t _reserved0;        ///<                                      +50
    uint32_t crc32_header;      ///< CRC32 of bytes [0..51] (self-excl.) +52

    uint8_t  _padding[8];       ///< Reserved for future use             +56
                                //                                        = 64 bytes total

    // ── Helpers ─────────────────────────────────────────────────────────────
    [[nodiscard]] bool hasMagic() const noexcept
    {
        return magic[0] == 'L' && magic[1] == 'O' &&
               magic[2] == 'O' && magic[3] == 'P';
    }

    void init() noexcept
    {
        magic[0] = 'L'; magic[1] = 'O'; magic[2] = 'O'; magic[3] = 'P';
        version_major = kVersionMajor;
        version_minor = kVersionMinor;
        tpqn          = kDefaultTPQN;
        _reserved0    = 0;
        crc32_header  = 0;
        sample_count  = 0;
        event_count   = 0;
        lut_offset = sample_data_offset = sequence_offset = meta_offset = 0;
        std::memset(_padding, 0, sizeof(_padding));
    }
};
static_assert(sizeof(FileHeader) == 64,
    "FileHeader must be exactly 64 bytes. Check struct members.");


// ┌─────────────────────────────────────────────────────────────────────────┐
// │  SampleLUTEntry   (32 bytes exactly)                                    │
// │  Lives in a contiguous array immediately after FileHeader.              │
// │  Allows O(1) lookup by SampleID without scanning sample data.           │
// └─────────────────────────────────────────────────────────────────────────┘
struct SampleLUTEntry
{
    uint16_t sample_id;         ///< Unique ID [0 .. sample_count-1]     +0
    uint8_t  channels;          ///< 1 = mono, 2 = stereo                +2
    uint8_t  flags;             ///< kBitDepth16 | kBitDepth24 | etc.    +3

    uint32_t sample_rate;       ///< e.g. 44100, 48000                   +4
    uint32_t frame_count;       ///< Total PCM frames in this sample     +8
    uint32_t byte_length;       ///< Raw byte size of PCM payload        +12

    uint64_t file_offset;       ///< Absolute offset into file for PCM   +16

    float    base_note;         ///< Root note in MIDI (e.g. 60.0 = C4)  +24
    float    loop_bpm;          ///< Original BPM of the loop (0=free)   +28
                                //                                        = 32 bytes

    // ── Helpers ─────────────────────────────────────────────────────────────
    [[nodiscard]] uint8_t bitDepth() const noexcept
    {
        if (flags & kBitDepth24) return 24;
        if (flags & kBitDepth16) return 16;
        return 0; // invalid
    }

    [[nodiscard]] uint32_t bytesPerFrame() const noexcept
    {
        return channels * (bitDepth() / 8u);
    }
};
static_assert(sizeof(SampleLUTEntry) == 32,
    "SampleLUTEntry must be exactly 32 bytes.");


// ┌─────────────────────────────────────────────────────────────────────────┐
// │  SampleDataBlock header  (16 bytes)                                     │
// │  Immediately precedes the raw PCM bytes for each sample.                │
// │  The actual PCM follows directly: [SampleDataBlock][PCM bytes...]       │
// └─────────────────────────────────────────────────────────────────────────┘
struct SampleDataBlock
{
    uint32_t tag;           ///< kTagSampleData FourCC for sanity check   +0
    uint16_t sample_id;     ///< Must match the LUT entry                 +4
    uint8_t  _reserved[2];  ///<                                          +6
    uint32_t byte_length;   ///< PCM payload byte count (follows this)    +8
    uint32_t crc32_pcm;     ///< CRC32 of the raw PCM bytes               +12
                            //                                             = 16 bytes
};
static_assert(sizeof(SampleDataBlock) == 16,
    "SampleDataBlock must be exactly 16 bytes.");


// ┌─────────────────────────────────────────────────────────────────────────┐
// │  SequenceHeader  (32 bytes)                                             │
// │  Sits at sequence_offset; followed by SequenceEvent[event_count].      │
// └─────────────────────────────────────────────────────────────────────────┘
struct SequenceHeader
{
    uint32_t tag;               ///< kTagSequence FourCC                  +0
    uint32_t event_count;       ///< Number of SequenceEvents that follow +4
    uint32_t total_duration_ticks; ///< Full sequence length in ticks     +8
    uint16_t time_sig_numerator;   ///< e.g. 4 (for 4/4)                 +12
    uint8_t  time_sig_denominator; ///< e.g. 4 (for 4/4) – power of 2   +14
    uint8_t  _reserved0;                                                //+15
    float    bpm;               ///< Master tempo (can be overridden)     +16
    uint32_t crc32_events;      ///< CRC32 over all SequenceEvents        +20
    uint8_t  _padding[8];       ///< Future use                           +24
                                //                                        = 32 bytes
};
static_assert(sizeof(SequenceHeader) == 32,
    "SequenceHeader must be exactly 32 bytes.");


// ┌─────────────────────────────────────────────────────────────────────────┐
// │  SequenceEvent  (8 bytes — single 64-bit word, bit-packed)              │
// │                                                                         │
// │  Bit layout (LSB → MSB):                                               │
// │                                                                         │
// │   Bits  0-27  : tick_start      (28 bits) → max ~268M ticks            │
// │                  @ TPQN=960, 120BPM: ~2330 minutes of music            │
// │   Bits 28-39  : sample_id       (12 bits) → 0..4095 samples            │
// │   Bits 40-46  : velocity        ( 7 bits) → 0..127 (MIDI-compatible)   │
// │   Bits 47-61  : loop_duration   (15 bits) → 0..32767 ticks             │
// │                  At 120BPM: ~34 bars (enough for any loop event)       │
// │   Bit  62     : one_shot  (play sample to end; ignore loop_duration)    │
// │   Bit  63     : muted     (event is silent)                             │
// └─────────────────────────────────────────────────────────────────────────┘
struct SequenceEvent
{
    uint64_t data; ///< All fields packed into 64 bits

    // ── Field widths & shifts ───────────────────────────────────────────────
    static constexpr uint64_t kTickShift          = 0;
    static constexpr uint64_t kTickBits           = 28;
    static constexpr uint64_t kTickMask           = (1ULL << kTickBits) - 1;

    static constexpr uint64_t kSampleIDShift      = 28;
    static constexpr uint64_t kSampleIDBits       = 12;
    static constexpr uint64_t kSampleIDMask       = (1ULL << kSampleIDBits) - 1;

    static constexpr uint64_t kVelocityShift      = 40;
    static constexpr uint64_t kVelocityBits       = 7;
    static constexpr uint64_t kVelocityMask       = (1ULL << kVelocityBits) - 1;

    // Loop duration: 15 bits → max 32,767 ticks
    // At TPQN=960, 120 BPM: max ~34 bars — more than enough for any loop event.
    static constexpr uint64_t kLoopDurShift       = 47;
    static constexpr uint64_t kLoopDurBits        = 15;
    static constexpr uint64_t kLoopDurMask        = (1ULL << kLoopDurBits) - 1;

    // Flags at bits 62 and 63 — the top 2 bits of uint64_t, shift is valid (< 64)
    static constexpr uint64_t kFlagOneShotShift   = 62;
    static constexpr uint64_t kFlagMutedShift     = 63;

    // ── Accessors ───────────────────────────────────────────────────────────
    LOOP_FORCE_INLINE
    [[nodiscard]] uint32_t tickStart() const noexcept
    {
        return static_cast<uint32_t>((data >> kTickShift) & kTickMask);
    }

    LOOP_FORCE_INLINE
    [[nodiscard]] uint16_t sampleID() const noexcept
    {
        return static_cast<uint16_t>((data >> kSampleIDShift) & kSampleIDMask);
    }

    LOOP_FORCE_INLINE
    [[nodiscard]] uint8_t velocity() const noexcept
    {
        return static_cast<uint8_t>((data >> kVelocityShift) & kVelocityMask);
    }

    LOOP_FORCE_INLINE
    [[nodiscard]] uint32_t loopDuration() const noexcept
    {
        return static_cast<uint32_t>((data >> kLoopDurShift) & kLoopDurMask);
    }

    LOOP_FORCE_INLINE
    [[nodiscard]] bool isOneShot() const noexcept
    {
        return ((data >> kFlagOneShotShift) & 1ULL) != 0ULL;
    }

    LOOP_FORCE_INLINE
    [[nodiscard]] bool isMuted() const noexcept
    {
        return ((data >> kFlagMutedShift) & 1ULL) != 0ULL;
    }

    // ── Mutators ────────────────────────────────────────────────────────────
    void set(uint32_t tick,
             uint16_t sampleId,
             uint8_t  vel,
             uint32_t durTicks,
             bool     oneShot = false,
             bool     muted   = false) noexcept
    {
        // Validate ranges in debug builds
        assert(tick     <= kTickMask);
        assert(sampleId <= kSampleIDMask);
        assert(vel      <= kVelocityMask);
        assert(durTicks <= kLoopDurMask);

        data = (static_cast<uint64_t>(tick     & kTickMask)     << kTickShift)
             | (static_cast<uint64_t>(sampleId & kSampleIDMask) << kSampleIDShift)
             | (static_cast<uint64_t>(vel      & kVelocityMask) << kVelocityShift)
             | (static_cast<uint64_t>(durTicks & kLoopDurMask)  << kLoopDurShift)
             | (oneShot ? (1ULL << kFlagOneShotShift) : 0ULL)
             | (muted   ? (1ULL << kFlagMutedShift)   : 0ULL);
    }
};
static_assert(sizeof(SequenceEvent) == 8,
    "SequenceEvent must be exactly 8 bytes (one 64-bit word).");


// ┌─────────────────────────────────────────────────────────────────────────┐
// │  MetaChunk header  (16 bytes)                                           │
// │  Followed by meta_byte_length bytes of UTF-8 "key=value\n" pairs.      │
// └─────────────────────────────────────────────────────────────────────────┘
struct MetaChunkHeader
{
    uint32_t tag;               ///< kTagMeta FourCC                      +0
    uint32_t meta_byte_length;  ///< Byte count of UTF-8 payload          +4
    uint32_t entry_count;       ///< Number of key=value pairs            +8
    uint32_t _reserved;         ///<                                      +12
                                //                                        = 16 bytes
};
static_assert(sizeof(MetaChunkHeader) == 16,
    "MetaChunkHeader must be exactly 16 bytes.");

} // namespace LoopFormat

LOOP_PACKED_END


// ─────────────────────────────────────────────────────────────────────────────
//  Utility Functions (header-only, constexpr / inline)
// ─────────────────────────────────────────────────────────────────────────────
namespace LoopFormat
{

// ── Timing Conversions ───────────────────────────────────────────────────────

/**
 * Convert a tick position to an absolute sample frame position.
 *
 * @param tick        Tick position (TPQN-based)
 * @param bpm         Current tempo in BPM
 * @param sampleRate  Audio sample rate (e.g. 44100.0)
 * @param tpqn        Ticks per quarter note (default 960)
 * @return            Sample frame index (suitable for buffer addressing)
 *
 * Formula:  samples = tick × (sampleRate × 60) / (bpm × tpqn)
 */
[[nodiscard]] LOOP_FORCE_INLINE
constexpr double tickToSample(uint32_t tick,
                               double   bpm,
                               double   sampleRate,
                               uint16_t tpqn = kDefaultTPQN) noexcept
{
    // Guard against division by zero in constexpr context
    if (bpm <= 0.0 || tpqn == 0) return 0.0;
    return static_cast<double>(tick) * (sampleRate * 60.0) / (bpm * static_cast<double>(tpqn));
}

/**
 * Convert a sample frame position back to ticks.
 */
[[nodiscard]] LOOP_FORCE_INLINE
constexpr uint32_t sampleToTick(double   samplePos,
                                 double   bpm,
                                 double   sampleRate,
                                 uint16_t tpqn = kDefaultTPQN) noexcept
{
    if (sampleRate <= 0.0 || bpm <= 0.0) return 0u;
    const double ticks = samplePos * (bpm * static_cast<double>(tpqn)) / (sampleRate * 60.0);
    return static_cast<uint32_t>(ticks + 0.5); // round to nearest tick
}

/**
 * Compute "samples per tick" for the processBlock look-ahead budget.
 */
[[nodiscard]] LOOP_FORCE_INLINE
constexpr double samplesPerTick(double   bpm,
                                 double   sampleRate,
                                 uint16_t tpqn = kDefaultTPQN) noexcept
{
    if (bpm <= 0.0 || tpqn == 0) return 0.0;
    return (sampleRate * 60.0) / (bpm * static_cast<double>(tpqn));
}


// ── Zero-Crossing Finder ─────────────────────────────────────────────────────

/**
 * Scan backwards from `endFrame` to find the nearest zero-crossing frame.
 * A zero-crossing is defined as a transition where |sample| < threshold.
 *
 * Use this in the Creator Tool when writing SampleLUTEntry::frame_count.
 *
 * @param pcm16      Pointer to interleaved 16-bit PCM
 * @param channels   Channel count (1 or 2)
 * @param endFrame   Starting scan point (typically the natural loop end)
 * @param maxLookback Max frames to scan backwards (default = half a second @ 44100)
 * @param threshold  Amplitude below which a sample is "at zero" (default 64 / 32768)
 * @return           Frame index of the nearest zero-crossing, or endFrame on failure
 *
 * NOTE: For 24-bit audio, widen threshold accordingly before calling.
 */
[[nodiscard]] inline
uint32_t findZeroCrossing_16(const int16_t* pcm16,
                              uint8_t        channels,
                              uint32_t       endFrame,
                              uint32_t       maxLookback = 22050u,
                              int16_t        threshold   = 64) noexcept
{
    if (!pcm16 || channels == 0 || endFrame == 0) return endFrame;

    const uint32_t scanStart = (endFrame > maxLookback)
                               ? (endFrame - maxLookback) : 0u;

    for (uint32_t f = endFrame; f > scanStart; --f)
    {
        bool allNearZero = true;
        for (uint8_t ch = 0; ch < channels; ++ch)
        {
            const int16_t s = pcm16[(f - 1u) * channels + ch];
            if (s > threshold || s < -threshold)
            {
                allNearZero = false;
                break;
            }
        }
        if (allNearZero) return f;
    }
    return endFrame; // fallback: use the original end
}


// ── CRC-32 (ISO 3309 / ITU-T V.42)  ─────────────────────────────────────────
/**
 * Compact header-only CRC-32 implementation.
 * Used to verify FileHeader and PCM payload integrity on load.
 */
[[nodiscard]] inline
uint32_t crc32(const void* data, size_t len, uint32_t crc = 0xFFFFFFFFu) noexcept
{
    // Pre-computed table for poly 0xEDB88320 (reflected 0x04C11DB7)
    static constexpr uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };

    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
    {
        crc = (crc >> 4) ^ table[(crc ^ (p[i]     )) & 0x0Fu];
        crc = (crc >> 4) ^ table[(crc ^ (p[i] >> 4)) & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}


// ── File Version Compatibility  ──────────────────────────────────────────────
/**
 * Returns true if a file with the given version can be read by this library.
 * Major version must match; minor version of file may be <= library minor.
 */
[[nodiscard]] LOOP_FORCE_INLINE
constexpr bool isVersionCompatible(uint16_t fileMajor,
                                    uint16_t fileMinor) noexcept
{
    return (fileMajor == kVersionMajor) && (fileMinor <= kVersionMinor);
}

} // namespace LoopFormat


// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations for JUCE-layer classes  (implementations in .cpp)
// ─────────────────────────────────────────────────────────────────────────────

// Defined in LoopFileReader.cpp
namespace LoopFormat { class FileReader; }

// Defined in LoopFileWriter.cpp  (Creator/Compiler Tool)
namespace LoopFormat { class FileWriter; }

// Defined in LoopSequencer.cpp   (AudioSource subclass)
namespace LoopFormat { class LoopSequencer; }

// Defined in ActiveVoice.h       (per-voice playback state, lock-free)
namespace LoopFormat { struct ActiveVoice; }

/*
 * ─────────────────────────────────────────────────────────────────────────────
 *  ROADMAP  —  JUCE AudioProcessor Implementation
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *  PHASE 1 · File I/O Layer  (LoopFileReader / LoopFileWriter)
 *  ────────────────────────────────────────────────────────────
 *  LoopFileReader
 *    ├─ Open with juce::MemoryMappedFile (READ_ONLY, whole-file map)
 *    ├─ Validate magic bytes + CRC32 of FileHeader
 *    ├─ Check version compatibility via isVersionCompatible()
 *    ├─ Build in-memory index:  std::array<SampleLUTEntry, kMaxSamples>
 *    │    → O(1) lookup by SampleID, avoids scanning the file
 *    ├─ Expose getSampleView(id) → juce::MemoryMappedFile::Range
 *    │    → returns a raw pointer + byte_length into the mmap'd region
 *    │    → NO copy to heap; audio thread reads directly from mapped pages
 *    └─ Validate CRC32 of each SampleDataBlock lazily (on first access)
 *
 *  LoopFileWriter  (Creator Tool only; never used by the Player)
 *    ├─ accumulate WAV inputs via addSample(juce::AudioBuffer<float>, meta)
 *    │    → convert float → int16/int24 in-place
 *    │    → call findZeroCrossing_16() to trim loop end
 *    │    → compute CRC32 of raw PCM bytes
 *    ├─ accumulate SequenceEvents from the Timeline grid
 *    ├─ write()  → serialise ALL structs in one sequential pass
 *    │    → compute & patch all offsets in FileHeader at the end
 *    │    → write CRC32 of FileHeader last
 *    └─ supports incremental "recompile" by re-writing sequence chunk only
 *       when no sample content has changed
 *
 *
 *  PHASE 2 · Sequencer Engine  (LoopSequencer : juce::AudioSource)
 *  ─────────────────────────────────────────────────────────────────
 *  Master Clock
 *    ├─ std::atomic<double> masterBpm   (UI thread writes, audio reads)
 *    ├─ std::atomic<uint64_t> playheadSample  (monotonic, never resets)
 *    ├─ playheadTick = sampleToTick(playheadSample, bpm, sampleRate)
 *    └─ tickFraction  (double sub-tick accumulator for sample-accurate
 *                      trigger scheduling, avoids integer rounding drift)
 *
 *  Look-ahead Scheduler
 *    ├─ kLookAheadTicks = 2 (≈ 1ms @ 120BPM/44100 = safety margin)
 *    ├─ On each getNextAudioBlock() call:
 *    │    window = [playheadTick, playheadTick + blockSizeTicks + kLookAheadTicks]
 *    │    binary-search SequenceEvent[] for events in window
 *    │    (array is pre-sorted by tickStart by the compiler tool)
 *    ├─ For each found event:
 *    │    triggerSampleOffset = tickToSample(evt.tick - playheadTick) → int
 *    │    push ActiveVoice into a juce::AbstractFifo ring (lock-free)
 *    └─ No mutex, no heap allocation; all state in stack-allocated structs
 *
 *  ActiveVoice  (per-playing-loop state)
 *    struct ActiveVoice {
 *        const int16_t* pcmPtr;    // direct pointer into mmap'd file
 *        uint32_t       frameCount;
 *        uint32_t       readPos;   // current frame position
 *        float          gainLinear;// velocity → gain (lookup table)
 *        uint8_t        channels;
 *        uint8_t        bitDepth;
 *        bool           loop;
 *        bool           active;
 *    };
 *    Pool: std::array<ActiveVoice, 64> voicePool (64 simultaneous loops)
 *
 *  Summing Mixer  (the processBlock inner loop)
 *    ├─ Iterate active voices; convert PCM → float in a tight SIMD-friendly loop
 *    ├─ Sum into a local stack buffer (float[2][blockSize])
 *    ├─ Apply soft-knee limiter (single-pass, coefficient table)
 *    │    threshold = 0.95f; ratio = 4:1 above threshold
 *    ├─ Copy to juce::AudioBuffer output
 *    └─ ZERO heap allocation: all buffers either stack or pre-allocated
 *       in prepareToPlay() and reused every block.
 *
 *
 *  PHASE 3 · Creator Tool GUI
 *  ──────────────────────────
 *  TimelineComponent : juce::Component
 *    ├─ Grid: rows = SampleIDs, columns = Beats (each column = 1 beat = TPQN ticks)
 *    ├─ Cells store optional SequenceEvent data
 *    ├─ Drag-and-drop WAV files onto rows → triggers addSample()
 *    ├─ Right-click cell → set velocity, duration, one-shot flag
 *    └─ "Compile" button → LoopFileWriter::write() → saves .LOOP file
 *
 *  WaveformThumbnailComponent : juce::AudioThumbnail
 *    └─ Shows zero-crossing trim point as a vertical green line
 *
 *  TransportBar
 *    └─ BPM spinner, Time Sig selector, Play/Stop → sets atomic bpm
 *
 *
 *  PHASE 4 · Player (Standalone / Plugin)
 *  ────────────────────────────────────────
 *  LoopPlayerAudioProcessor : juce::AudioProcessor
 *    ├─ prepareToPlay()  → LoopFileReader::open(), build voice pool
 *    ├─ processBlock()   → LoopSequencer::getNextAudioBlock()
 *    ├─ MIDI input       → velocity-override for live performance
 *    └─ Parameter tree   → BPM, Master Volume, Per-Sample Gain (automatable)
 *
 *  juce::MemoryMappedFile strategy
 *    ├─ Map the ENTIRE file once in prepareToPlay()
 *    ├─ ActiveVoice::pcmPtr = mapped_base + lut_entry.file_offset
 *    │    + sizeof(SampleDataBlock)
 *    └─ OS page-fault loads only the pages actually played,
 *       keeping RAM usage proportional to active voices, not file size.
 *
 *
 *  BUILD CONFIGURATION NOTES  (Visual Studio 2022)
 *  ─────────────────────────────────────────────────
 *  · C++ Language Standard : /std:c++20
 *  · Optimisation           : /O2 /GL (Release), /Od (Debug)
 *  · SIMD                   : /arch:AVX2  (or AVX512 if targeting modern HW)
 *  · Warning level          : /W4 /WX
 *  · Whole-program optim.   : /LTCG in Release
 *  · Static analysis        : /analyze (catches signed/unsigned UB in bit ops)
 *  · Address Sanitizer      : /fsanitize=address in Debug config
 *  · Preprocessor defines   :
 *      JUCE_STRICT_REFCOUNTEDPOINTER=1
 *      JUCE_USE_VDSP_FRAMEWORK=0  (Windows – disable Apple DSP)
 *      LOOP_AUDIO_BLOCK_SIZE=512  (override in project settings)
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <fstream>
#include <string>
#include <mutex>

namespace LoopFormat
{
    inline void logDebug(const std::string& msg)
    {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        std::ofstream logFile("debug.log", std::ios::app);
        if (logFile.is_open())
        {
            logFile << msg << std::endl;
        }
    }
}
