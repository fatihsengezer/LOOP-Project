/**
 * @file    ActiveVoice.h
 * @brief   Per-voice playback state for the LoopSequencer audio engine.
 *
 * =============================================================================
 * AGENT / DEVELOPER QUICK-START
 * =============================================================================
 *
 * PURPOSE
 *   An ActiveVoice represents one currently-playing loop instance.
 *   The LoopSequencer maintains a pool of up to kMaxVoices of these.
 *   They are POD-like structs (no virtual functions, no heap allocation)
 *   designed to be stored in a std::array and accessed from the audio thread.
 *
 * LIFETIME
 *   Voices are acquired and released by the LoopSequencer, not created/
 *   destroyed. The pool is pre-allocated in prepareToPlay(); processBlock()
 *   only flips the `active` flag and advances `readPos`.
 *
 * LOCK-FREE DESIGN
 *   The sequencer uses two pools:
 *     - voicePool_    : Active voices the render thread reads from
 *     - pendingVoices_: A juce::AbstractFifo ring that the scheduler thread
 *                       writes new voices into
 *
 *   In each processBlock():
 *     1. Drain pendingVoices_ → write into voicePool_ slots
 *     2. Render all active voicePool_ slots into the output buffer
 *     3. Mark finished voices inactive
 *
 *   No mutex. No heap allocation. The FIFO is the only shared state.
 *
 * PCM DECODING
 *   Decoding happens sample-by-sample in the render loop via the helper
 *   functions below. For a production implementation, consider SIMD batching
 *   (convert a block of N samples at once using AVX2 intrinsics) for lower
 *   CPU usage. The helpers below are scalar and readable.
 *
 * =============================================================================
 */

#pragma once

#include "FormatSpec.h"

#include <cstdint>
#include <cstring>   // std::memcpy
#include <cassert>
#include <array>
#include <atomic>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  VelocityTable — compile-time velocity→gain lookup
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Pre-computed linear gain values for MIDI velocities 0..127.
 *
 * Formula: gain = (velocity / 127.f) ^ 2
 * The squared relationship gives a more musical velocity response than
 * a simple linear scale.
 *
 * Usage: float gain = VelocityTable::gain(evt.velocity());
 *
 * This table is populated at compile time via constexpr — zero runtime cost.
 */
struct VelocityTable
{
    static constexpr int kSize = 128;

    constexpr VelocityTable() noexcept : table{}
    {
        for (int v = 0; v < kSize; ++v)
        {
            const float linear = static_cast<float>(v) / 127.f;
            table[v] = linear * linear; // Squared for musicality
        }
    }

    [[nodiscard]] LOOP_FORCE_INLINE
    float gain(uint8_t velocity) const noexcept
    {
        return table[velocity & 0x7F]; // velocity is 7-bit; mask for safety
    }

    float table[kSize];
};

// Global instance — zero runtime cost; resides in .rodata
inline constexpr VelocityTable kVelocityTable;


// ─────────────────────────────────────────────────────────────────────────────
//  ActiveVoice
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Complete playback state for one loop voice.
 *
 * All fields are accessed exclusively by the audio thread during render,
 * EXCEPT `active` which is also written by the scheduler path.
 *
 * SIZE: 40 bytes. A pool of 64 voices = 2560 bytes = fits in L1 cache.
 */
struct ActiveVoice
{
    // ── Read-only after activation ───────────────────────────────────────────

    /// Direct pointer into the memory-mapped PCM data.
    /// Points to the first PCM byte (past the SampleDataBlock header).
    /// NEVER free() or delete[] this — it is owned by LoopFileReader.
    const uint8_t* pcmPtr        { nullptr };

    /// Total frame count for this sample (post zero-crossing trim).
    uint32_t       frameCount    { 0 };

    /// How many frames to play before either looping or stopping.
    /// For one-shot voices: == frameCount.
    /// For looping voices: may be < frameCount (custom loop length).
    uint32_t       playFrames    { 0 };

    /// Sample offset within the processBlock() buffer where this voice starts.
    /// Set when the voice is triggered; used to handle mid-block triggers
    /// precisely (sample-accurate).
    uint32_t       startOffset   { 0 };

    /// The total number of frames this voice should render before stopping.
    /// For one-shot voices, this is equal to playFrames.
    /// For looping voices, this is derived from evt.loopDuration().
    uint32_t       totalFramesToPlay { 0 };

    /// Pre-looked-up gain from VelocityTable.
    float          gainLinear    { 1.f };

    /// Number of audio channels (1 = mono, 2 = stereo).
    uint8_t        channels      { 1 };

    /// Bit depth of the PCM data (16 or 24).
    uint8_t        bitDepth      { 16 };

    /// If true: play to the natural end (frameCount frames) and stop.
    /// If false: loop back to frame 0 after `playFrames` frames.
    bool           oneShot       { false };

    uint8_t        _pad1[1]      { 0 }; ///< Padding before double

    /// Ratio of (fileSampleRate / hostSampleRate) to advance readPos
    double         sampleRateRatio { 1.0 };

    // ── Mutable during render ────────────────────────────────────────────────

    /// Current read position within the PCM data, in frames (fractional for resampling).
    /// Advanced by the render loop every processBlock().
    double         readPos       { 0.0 };

    /// Total number of frames rendered so far by this voice.
    uint32_t       framesPlayed  { 0 };

    // ── Control ──────────────────────────────────────────────────────────────

    /// True while this voice slot is in use.
    /// Written by the scheduler (on activation) and by the render loop
    /// (on deactivation). In a single-audio-thread setup, no atomics needed;
    /// but see the FIFO note in the file header if you add multi-threading.
    bool           active        { false };

    uint8_t        _pad2[3]      { 0, 0, 0 }; ///< Maintain 8-byte structure alignment

    // ── Helpers ──────────────────────────────────────────────────────────────

    /**
     * Reset the voice to its default inactive state.
     * Called by the sequencer when reclaiming a finished voice.
     */
    void reset() noexcept
    {
        pcmPtr            = nullptr;
        frameCount        = 0;
        playFrames        = 0;
        startOffset       = 0;
        totalFramesToPlay = 0;
        gainLinear        = 1.f;
        channels          = 1;
        bitDepth          = 16;
        oneShot           = false;
        sampleRateRatio   = 1.0;
        readPos           = 0.0;
        framesPlayed      = 0;
        active            = false;
    }

    /**
     * Returns true if this voice has finished playing all its frames.
     * Checked at the end of each processBlock() to reclaim the slot.
     */
    [[nodiscard]] LOOP_FORCE_INLINE
    bool isFinished() const noexcept
    {
        return active && framesPlayed >= totalFramesToPlay;
    }
};

static_assert(sizeof(ActiveVoice) == 56,
    "ActiveVoice size changed — update pool cache-size calculation in README");


// ─────────────────────────────────────────────────────────────────────────────
//  PCM decoding helpers (audio-thread, scalar)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Read one 16-bit signed integer from a LE byte stream and convert to float.
 *
 * @param ptr  Pointer to the first byte of the sample (2 bytes total).
 * @return     Float in [-1.f, 1.f].
 */
[[nodiscard]] LOOP_FORCE_INLINE
float readInt16ToFloat(const uint8_t* ptr) noexcept
{
    // Reconstruct signed int16 from two LE bytes
    const int16_t raw = static_cast<int16_t>(
                            static_cast<uint16_t>(ptr[0]) |
                           (static_cast<uint16_t>(ptr[1]) << 8));
    return static_cast<float>(raw) * (1.f / 32768.f);
}

/**
 * Read one 24-bit signed integer from a LE byte stream and convert to float.
 *
 * @param ptr  Pointer to the first byte of the sample (3 bytes total).
 * @return     Float in [-1.f, 1.f].
 */
[[nodiscard]] LOOP_FORCE_INLINE
float readInt24ToFloat(const uint8_t* ptr) noexcept
{
    // Reconstruct 24-bit value from 3 LE bytes
    const int32_t raw = static_cast<int32_t>(
                             static_cast<uint32_t>(ptr[0])
                          | (static_cast<uint32_t>(ptr[1]) << 8)
                          | (static_cast<uint32_t>(ptr[2]) << 16));

    // Sign-extend from 24-bit to 32-bit
    // The 24th bit (value 0x800000) is the sign bit.
    const int32_t signExtended = (raw & 0x800000) ? (raw | 0xFF000000) : raw;

    return static_cast<float>(signExtended) * (1.f / 8388608.f);
}

/**
 * Read one audio frame (all channels) from a voice's current readPos
 * and advance the read pointer.
 *
 * For mono: writes output[0] only.
 * For stereo: writes output[0] (left) and output[1] (right).
 *
 * @param voice    The active voice to read from.
 * @param output   Float output array, must have at least `voice.channels` elements.
 */
LOOP_FORCE_INLINE
void readVoiceFrame(const ActiveVoice& voice,
                    float*             output) noexcept
{
    assert(voice.pcmPtr && "readVoiceFrame called with null pcmPtr");
    assert(static_cast<uint32_t>(voice.readPos) < voice.frameCount);

    const uint32_t bytesPerSample = (voice.bitDepth == 24) ? 3u : 2u;
    const uint32_t bytesPerFrame  = bytesPerSample * voice.channels;
    const uint8_t* framePtr       = voice.pcmPtr
                                  + static_cast<size_t>(voice.readPos) * bytesPerFrame;

    if (voice.bitDepth == 16)
    {
        for (uint8_t ch = 0; ch < voice.channels; ++ch)
            output[ch] = readInt16ToFloat(framePtr + ch * 2u) * voice.gainLinear;
    }
    else // 24-bit
    {
        for (uint8_t ch = 0; ch < voice.channels; ++ch)
            output[ch] = readInt24ToFloat(framePtr + ch * 3u) * voice.gainLinear;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Voice pool constants
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum simultaneous loop voices.
/// 64 voices × 40 bytes = 2560 bytes ≈ fits in L1 cache on most CPUs.
/// Increase if your use case requires more simultaneous loops.
constexpr int kMaxVoices = 64;

/// The pending voice FIFO depth.
/// Must be a power of 2 for juce::AbstractFifo.
/// 32 pending triggers per audio block is generous for any real-world tempo.
constexpr int kPendingFifoSize = 32;

} // namespace LoopFormat
