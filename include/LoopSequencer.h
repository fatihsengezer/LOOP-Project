/**
 * @file    LoopSequencer.h
 * @brief   Tick-accurate loop sequencer engine — inherits juce::AudioSource.
 *
 * =============================================================================
 * AGENT / DEVELOPER QUICK-START
 * =============================================================================
 *
 * PURPOSE
 *   LoopSequencer is the real-time heart of the Player. It:
 *     1. Maintains a master clock (tick-based, BPM-driven)
 *     2. Scans the SequenceEvent array for upcoming triggers (look-ahead)
 *     3. Activates voices in a lock-free voice pool
 *     4. Mixes all active voices into the output AudioBuffer
 *
 *   It inherits juce::AudioSource, so it can be used:
 *     - Directly in a juce::AudioSourcePlayer (standalone app)
 *     - Driven by LoopPlayerProcessor::processBlock() (plugin)
 *
 * THREADING MODEL (CRITICAL — read before editing)
 * ─────────────────────────────────────────────────
 *   There are exactly TWO threads that interact with this class:
 *
 *   ① MESSAGE THREAD  — calls: setBPM(), transport controls (play/stop/seek)
 *   ② AUDIO THREAD    — calls: getNextAudioBlock(), prepareToPlay(),
 *                              releaseResources()
 *
 *   The bridge between them is a set of std::atomics.
 *   Rules:
 *     - Audio thread NEVER allocates heap memory (no new/delete/vector resize)
 *     - Audio thread NEVER acquires a mutex
 *     - Message thread writes to atomics with memory_order_release
 *     - Audio thread reads atomics with memory_order_acquire
 *
 *   The pending-voice FIFO (juce::AbstractFifo) is the only structure written
 *   by the scheduler path and read by the render path. It is lock-free by design.
 *
 * LOOK-AHEAD SCHEDULER
 * ─────────────────────
 *   On every getNextAudioBlock() call, the sequencer computes:
 *
 *     windowStart = playheadTick (current position)
 *     windowEnd   = playheadTick + blockSizeTicks + kLookAheadTicks
 *
 *   It binary-searches the (pre-sorted) SequenceEvent array for all events
 *   in this window, and activates a voice for each one. The per-event
 *   `startOffset` field carries the intra-block sample position where the
 *   voice should start contributing to the output.
 *
 *   kLookAheadTicks = 2 by default. At 120BPM / 44100Hz this is ~46 samples
 *   of look-ahead — enough to absorb scheduler jitter without audible delay.
 *
 * VOICE STEALING
 * ───────────────
 *   When all 64 voice slots are occupied and a new trigger arrives, the
 *   sequencer steals the oldest active voice (the one with the highest
 *   readPos / playFrames ratio). This is better than dropping the new event.
 *   In practice, well-authored sequences should never hit this limit.
 *
 * SUMMING MIXER
 * ──────────────
 *   All active voices are summed into a float accumulation buffer
 *   (pre-allocated in prepareToPlay, NOT stack-allocated per-block).
 *   After summing, a soft-knee limiter (coefficient table, no branching)
 *   prevents clipping. Output is written directly to the
 *   juce::AudioBuffer provided by getNextAudioBlock().
 *
 * MONO → STEREO UPMIX
 *   Mono samples are duplicated to both output channels.
 *   Stereo samples with a mono output channel use (L+R)/2.
 *
 * =============================================================================
 */

#pragma once

#include "FormatSpec.h"
#include "LoopFileReader.h"
#include "ActiveVoice.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Transport state (message-thread writes, audio-thread reads)
// ─────────────────────────────────────────────────────────────────────────────

enum class TransportState : int
{
    Stopped  = 0,
    Playing  = 1,
    Paused   = 2,   ///< Clock frozen; voices continue to decay
};


// ─────────────────────────────────────────────────────────────────────────────
//  LoopSequencer
// ─────────────────────────────────────────────────────────────────────────────

class LoopSequencer  :  public juce::AudioSource
{
public:
    // ── Construction / destruction ───────────────────────────────────────────

    /**
     * @param reader  A fully open and validated LoopFileReader.
     *                The sequencer holds a non-owning reference — the caller
     *                is responsible for keeping the reader alive for the
     *                duration of the sequencer's use and calling
     *                releaseResources() before destroying the reader.
     */
    explicit LoopSequencer(LoopFileReader& reader);
    ~LoopSequencer() override;

    LoopSequencer(const LoopSequencer&)            = delete;
    LoopSequencer& operator=(const LoopSequencer&) = delete;


    // ── juce::AudioSource interface ──────────────────────────────────────────

    /**
     * Called by the audio device before streaming begins.
     * Allocates the mix accumulation buffer and resets the voice pool.
     * Must be called from the audio thread (or before it starts).
     *
     * @param samplesPerBlockExpected  Typical block size. The actual block
     *        size in getNextAudioBlock() may differ; the accumulation buffer
     *        is allocated to handle up to 2× this value.
     * @param sampleRate               The device sample rate.
     */
    void prepareToPlay(int samplesPerBlockExpected,
                       double sampleRate) override;

    /**
     * The audio render callback — called once per audio block.
     *
     * Steps:
     *   1. Drain the pending-voice FIFO → activate voices in voicePool_
     *   2. For each active voice: decode frames into the accumulation buffer
     *   3. Apply soft-knee limiter to the accumulation buffer
     *   4. Copy accumulation buffer → output AudioBuffer
     *   5. Advance playhead by blockSize samples
     *   6. Look-ahead: find events in the upcoming window, push to pending FIFO
     *
     * AUDIO THREAD CONTRACTS:
     *   - No heap allocation
     *   - No mutex acquisition
     *   - No blocking I/O
     *
     * @param bufferToFill  JUCE audio callback structure; fill
     *        bufferToFill.buffer with output audio.
     */
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

    /**
     * Called when the audio device stops.
     * Resets voice pool; does NOT free the accumulation buffer
     * (that is done in the destructor).
     */
    void releaseResources() override;


    // ── Transport controls (message thread) ──────────────────────────────────

    /** Start playback from the current position. */
    void play()  noexcept;

    /** Stop playback and reset the playhead to tick 0. */
    void stop()  noexcept;

    /** Pause without resetting the playhead. */
    void pause() noexcept;

    /**
     * Seek to a specific tick position.
     * Clears all active voices to prevent orphaned tails.
     *
     * @param tick  Target tick position (0 = start of sequence).
     */
    void seekToTick(uint32_t tick) noexcept;


    // ── Parameter control (message thread, atomic) ───────────────────────────

    /**
     * Set the master BPM.
     *
     * Takes effect at the start of the next audio block.
     * Does NOT recompute stored tick values — they are BPM-independent.
     *
     * Valid range: [20.f, 400.f]. Values outside this range are clamped.
     */
    void setBPM(float bpm) noexcept;

    [[nodiscard]] float getBPM() const noexcept
    {
        return bpm_.load(std::memory_order_acquire);
    }

    /**
     * Set the master output gain (linear, 0.f = silence, 1.f = unity).
     */
    void setMasterGain(float gain) noexcept
    {
        masterGain_.store(std::max(0.f, gain), std::memory_order_release);
    }

    /**
     * Enable or disable looping of the entire sequence.
     * When true, the playhead wraps to tick 0 when it reaches
     * SequenceHeader::total_duration_ticks.
     */
    void setLooping(bool shouldLoop) noexcept
    {
        looping_.store(shouldLoop, std::memory_order_release);
    }

    [[nodiscard]] TransportState getTransportState() const noexcept
    {
        return static_cast<TransportState>(
            transportState_.load(std::memory_order_acquire));
    }

    /**
     * Returns the current playhead position in ticks.
     * Audio-thread safe (reads a std::atomic).
     */
    [[nodiscard]] uint32_t getPlayheadTick() const noexcept
    {
        return playheadTick_.load(std::memory_order_acquire);
    }

    /** Returns the number of currently active voices (for metering / debug). */
    [[nodiscard]] int getActiveVoiceCount() const noexcept;


    // ── Sequencer configuration ───────────────────────────────────────────────

    /**
     * Number of extra ticks to look ahead when scheduling events.
     * Default: 2 ticks. Increase if you observe missed events at very
     * small buffer sizes (< 64 samples). Larger values add latency.
     */
    void setLookAheadTicks(uint32_t ticks) noexcept
    {
        lookAheadTicks_.store(ticks, std::memory_order_release);
    }

    /**
     * Set the per-sample-ID mute bitmask.
     *
     * Bit N = 1 → sample ID N is muted (new triggers are not scheduled).
     * Supports up to 64 unique sample IDs. IDs ≥ 64 are never affected.
     *
     * Written from the message thread (UI); read on the audio thread in
     * scheduleLookAhead(). Stored as a std::atomic<uint64_t>.
     */
    void setMutedMask(uint64_t mask) noexcept
    {
        mutedMask_.store(mask, std::memory_order_release);
    }

    [[nodiscard]] uint64_t getMutedMask() const noexcept
    {
        return mutedMask_.load(std::memory_order_acquire);
    }

    /** Returns the full sequence duration in ticks (for UI display). */
    [[nodiscard]] uint32_t getSequenceDurationTicks() const noexcept
    {
        return sequenceDurationTicks_;
    }


private:
    // ─────────────────────────────────────────────────────────────────────────
    //  Internal types
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Carries the data needed to activate a voice, passed through
     * the AbstractFifo from the scheduler to the render path.
     *
     * This is a POD struct — no constructors, no destructors.
     * Must fit in one AbstractFifo slot.
     */
    struct PendingTrigger
    {
        const uint8_t* pcmPtr     { nullptr }; ///< Into mmap'd file
        uint32_t       frameCount { 0 };
        uint32_t       playFrames { 0 };
        uint32_t       startOffset{ 0 };       ///< Intra-block trigger offset
        uint32_t       totalFramesToPlay { 0 };
        float          gainLinear { 1.f };
        uint8_t        channels   { 1 };
        uint8_t        bitDepth   { 16 };
        bool           oneShot    { false };
    };


    // ─────────────────────────────────────────────────────────────────────────
    //  Private methods
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Process look-ahead for one audio block.
     *
     * Computes the tick window [tickStart, tickEnd] that this block spans
     * (plus the look-ahead margin), performs a binary search for events
     * in that window, and pushes PendingTrigger structs into pendingFifo_.
     *
     * Called at the END of getNextAudioBlock() for the NEXT block
     * (classic double-buffering: schedule ahead, then render).
     *
     * @param blockStartTick  Tick at the start of the current/next block
     * @param blockSizeTicks  How many ticks one block spans (double)
     * @param blockSizeSamples Exact sample count of the block (for startOffset calc)
     */
    void scheduleLookAhead(uint32_t blockStartTick,
                           double   blockSizeTicks,
                           int      blockSizeSamples) noexcept;

    /**
     * Drain the pendingFifo_ and activate voices in the voice pool.
     * Called at the START of getNextAudioBlock() before rendering.
     */
    void drainPendingFifo() noexcept;

    /**
     * Find the first available (inactive) voice slot in voicePool_.
     * Returns nullptr if all slots are occupied — caller should steal.
     */
    [[nodiscard]] ActiveVoice* findFreeVoice() noexcept;

    /**
     * Steal the "least important" active voice when the pool is full.
     * Current policy: steal the voice with the highest playback completion
     * ratio (readPos / playFrames), i.e. the one closest to its natural end.
     */
    [[nodiscard]] ActiveVoice* stealVoice() noexcept;

    /**
     * Render all active voices into mixBuffer_ for one audio block.
     *
     * @param numSamples  Number of samples to render in this block.
     * @param numOutputCh Number of output channels (1 or 2).
     */
    void renderVoices(int numSamples, int numOutputCh) noexcept;

    /**
     * Apply a single-pass soft-knee limiter to mixBuffer_.
     *
     * Threshold: -0.5dBFS (0.944f linear)
     * Knee width: 0.2 (smooth transition zone)
     * Ratio above threshold: 4:1
     *
     * This is computed from a coefficient table built in prepareToPlay()
     * to avoid branching and division in the audio thread.
     *
     * @param numSamples  Number of samples to process.
     * @param numChannels Number of channels in mixBuffer_.
     */
    void applyLimiter(int numSamples, int numChannels) noexcept;

    /**
     * Convert a block size in samples to ticks (double precision to avoid
     * accumulation of rounding error across many blocks).
     */
    [[nodiscard]] double blockSizeToTicks(int numSamples) const noexcept;


    // ─────────────────────────────────────────────────────────────────────────
    //  State — audio-thread-owned (no atomics needed)
    //  These are written exclusively by prepareToPlay() / releaseResources()
    //  or from within getNextAudioBlock().
    // ─────────────────────────────────────────────────────────────────────────

    LoopFileReader& reader_;  ///< Non-owning reference

    /// The voice pool: pre-allocated, never heap-expanded during playback.
    /// Slot ownership: sequencer sets active=true to acquire, active=false to release.
    std::array<ActiveVoice, kMaxVoices> voicePool_ {};

    /**
     * The mix accumulation buffer. Pre-allocated in prepareToPlay() to
     * maxBlockSize × 2 channels. Cleared to zero at the start of each block,
     * then accumulated into by all active voices.
     *
     * Using juce::AudioBuffer here for JUCE ecosystem compatibility (it uses
     * JUCE's aligned allocator, which is SIMD-friendly).
     */
    juce::AudioBuffer<float> mixBuffer_;

    /// Playhead position as a fractional sample count (not ticks).
    /// Using double to prevent drift over long sessions.
    /// Updated every block; converted to ticks on demand.
    double playheadSample_ { 0.0 };

    /// Sub-tick accumulator — carries the fractional tick remainder between
    /// blocks to prevent tick-quantisation drift over thousands of blocks.
    double tickAccumulator_ { 0.0 };

    /// Cached values from prepareToPlay() — avoid atomic reads in hot path.
    double   sampleRate_       { 44100.0 };
    int      maxBlockSize_     { 512 };

    /// Pointer into the mmap'd event array (cached from reader_ in prepareToPlay)
    const SequenceEvent* eventsBegin_ { nullptr };
    const SequenceEvent* eventsEnd_   { nullptr };

    /// Total sequence duration in ticks (from SequenceHeader)
    uint32_t sequenceDurationTicks_ { 0 };

    /// The index into the event array of the next unscheduled event.
    /// Maintained across blocks to avoid redundant binary searches.
    uint32_t nextEventIndex_ { 0 };


    // ─────────────────────────────────────────────────────────────────────────
    //  State — shared between message thread and audio thread (atomics)
    // ─────────────────────────────────────────────────────────────────────────

    std::atomic<float>    bpm_            { 120.f };
    std::atomic<float>    masterGain_     { 1.f };
    std::atomic<int>      transportState_ { static_cast<int>(TransportState::Stopped) };
    std::atomic<bool>     looping_        { true };
    std::atomic<uint32_t> playheadTick_   { 0 };
    std::atomic<uint32_t> lookAheadTicks_ { 2 };

    /**
     * Per-sample-ID mute bitmask.
     * Bit N = 1 → sample ID N is muted (scheduleLookAhead skips its events).
     * Written by the message thread (UI), read by the audio thread.
     */
    std::atomic<uint64_t> mutedMask_      { 0 };

    /// Set by seekToTick() on the message thread.
    /// Checked at the top of getNextAudioBlock().
    std::atomic<bool>     seekPending_    { false };
    std::atomic<uint32_t> seekTargetTick_ { 0 };


    // ─────────────────────────────────────────────────────────────────────────
    //  Pending-voice FIFO (lock-free bridge: scheduler → render)
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * AbstractFifo manages the read/write indices lock-free.
     * The actual data lives in pendingTriggerData_ (a fixed-size array).
     *
     * Write side (scheduleLookAhead): called from getNextAudioBlock()
     *   → audio thread only — no contention
     * Read side  (drainPendingFifo):  called from getNextAudioBlock()
     *   → same thread — also no contention in single-audio-thread setup
     *
     * NOTE: If you ever move scheduling to a separate thread, you will need
     * to verify the AbstractFifo thread-safety guarantees apply to your setup.
     * juce::AbstractFifo is safe for 1 writer + 1 reader simultaneously.
     */
    juce::AbstractFifo pendingFifo_ { kPendingFifoSize };
    std::array<PendingTrigger, kPendingFifoSize> pendingTriggerData_ {};

    // ─────────────────────────────────────────────────────────────────────────
    //  Limiter coefficients (built in prepareToPlay, read-only during render)
    // ─────────────────────────────────────────────────────────────────────────

    static constexpr float kLimiterThreshold = 0.944f;  ///< ≈ -0.5 dBFS
    static constexpr float kLimiterRatio     = 4.f;
    static constexpr float kLimiterKneeWidth = 0.2f;

    // ─────────────────────────────────────────────────────────────────────────
    //  Constants
    // ─────────────────────────────────────────────────────────────────────────

    static constexpr float kMinBPM = 20.f;
    static constexpr float kMaxBPM = 400.f;
};

} // namespace LoopFormat
