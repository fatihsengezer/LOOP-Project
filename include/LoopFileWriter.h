/**
 * @file    LoopFileWriter.h
 * @brief   Creator-side serialiser: converts WAV buffers + timeline events
 *          into a single binary `.LOOP` file.
 *
 * =============================================================================
 * AGENT / DEVELOPER QUICK-START
 * =============================================================================
 *
 * PURPOSE
 *   This class is used exclusively by the Creator Tool (DAW-lite). It is
 *   never instantiated in the Player (runtime) path. Its job is to take:
 *     - N loaded WAV files (as juce::AudioBuffer<float>)
 *     - M SequenceEvents from the Timeline grid
 *   ...and write them to a packed, CRC-verified `.LOOP` binary file.
 *
 * TYPICAL USAGE (see also tests/test_format_roundtrip.cpp)
 * ---------------------------------------------------------
 *   LoopFileWriter writer;
 *
 *   // 1. Add samples (order defines their SampleID: first added = ID 0)
 *   SampleMeta kick_meta { .sample_rate = 44100, .channels = 1,
 *                          .bit_depth = 16, .base_note = 60.f, .loop_bpm = 120.f };
 *   writer.addSample(kick_buffer, kick_meta);   // → gets ID 0
 *   writer.addSample(snare_buffer, snare_meta); // → gets ID 1
 *
 *   // 2. Add sequence events
 *   LoopFormat::SequenceEvent evt;
 *   evt.set(0,    0, 100, 1920); // tick=0, id=0, vel=100, dur=1920 ticks
 *   evt.set(1920, 1,  90,  960); // tick=1920, id=1, vel=90, dur=960 ticks
 *   writer.addEvent(evt);
 *
 *   // 3. Compile to disk
 *   auto result = writer.write(juce::File("/path/to/output.loop"));
 *   if (result != LoopFileWriter::Error::None)
 *       // handle error
 *
 * THREAD SAFETY
 *   This class is NOT thread-safe. Call all methods from a single thread
 *   (the message thread). The `write()` call is synchronous and may take
 *   up to several seconds for large sample banks.
 *
 * WHAT THIS FILE DOES NOT DO
 *   - Does NOT decode WAV files — the caller provides a decoded AudioBuffer.
 *     Use juce::AudioFormatManager + juce::AudioFormatReader for WAV decoding.
 *   - Does NOT validate BPM/time-signature consistency between events — the
 *     caller is responsible.
 *   - Does NOT perform sample-rate conversion — all samples in one file must
 *     share a sample rate (enforced: addSample() returns SampleRateMismatch
 *     if a second sample with a different rate is added).
 *
 * DEPENDENCIES
 *   - FormatSpec.h   (binary struct definitions)
 *   - JUCE modules:  juce_core, juce_audio_basics
 *
 * =============================================================================
 */

#pragma once

#include "FormatSpec.h"

// JUCE includes — these come from the JUCE module system.
// In a Projucer project, they are auto-included via JuceHeader.h.
// In a CMake project, link target_link_libraries(... juce::juce_core juce::juce_audio_basics)
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>
#include <string>
#include <cstdint>
#include <optional>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  SampleMeta
//  Metadata the caller provides alongside each AudioBuffer<float>.
//  This is the *writer-side* description; the reader uses SampleLUTEntry.
// ─────────────────────────────────────────────────────────────────────────────
struct SampleMeta
{
    uint32_t    sample_rate { 44100 };  ///< Must match AudioBuffer content
    uint8_t     channels    { 1 };      ///< 1 = mono, 2 = stereo
    uint8_t     bit_depth   { 16 };     ///< 16 or 24 — determines output encoding
    float       base_note   { 60.f };   ///< MIDI root note (60 = C4)
    float       loop_bpm    { 0.f };    ///< Original BPM; 0 = free (non-tempo-sync'd)
    std::string name;                   ///< Optional; stored in MetaChunk as "sN.name=..."
};


// ─────────────────────────────────────────────────────────────────────────────
//  LoopFileWriter
// ─────────────────────────────────────────────────────────────────────────────
class LoopFileWriter
{
public:
    // ── Error Codes ──────────────────────────────────────────────────────────
    enum class Error
    {
        None = 0,
        TooManySamples,        ///< Exceeded kMaxSamples (4096)
        TooManyEvents,         ///< Exceeded kMaxEvents  (~1 million)
        InvalidBitDepth,       ///< bit_depth must be 16 or 24
        InvalidChannelCount,   ///< channels must be 1 or 2
        SampleRateMismatch,    ///< All samples in one file must share a rate
        BufferEmpty,           ///< AudioBuffer has zero frames
        FileOpenFailed,        ///< Could not create/open the output file
        WriteError,            ///< juce::FileOutputStream::write() returned false
        NoSamples,             ///< write() called before any samples were added
        InternalError,         ///< Should never happen; indicates a bug
    };

    /// Human-readable description of an error code (for logging/UI display).
    [[nodiscard]] static const char* describeError(Error e) noexcept;


    // ── Lifecycle ────────────────────────────────────────────────────────────

    LoopFileWriter()  = default;
    ~LoopFileWriter() = default;

    // Not copyable (owns heap-allocated PCM buffers)
    LoopFileWriter(const LoopFileWriter&)            = delete;
    LoopFileWriter& operator=(const LoopFileWriter&) = delete;

    // Movable
    LoopFileWriter(LoopFileWriter&&)            = default;
    LoopFileWriter& operator=(LoopFileWriter&&) = default;


    // ── Configuration ────────────────────────────────────────────────────────

    /**
     * @brief Set the master BPM for the sequence header.
     *
     * Default: 120.0. Must be called before write().
     * Does NOT affect how sample data is stored — samples are BPM-agnostic.
     */
    void setBPM(float bpm) noexcept { masterBpm_ = bpm; }

    /**
     * @brief Set the time signature for the sequence header.
     *
     * Default: 4/4.
     * @param numerator    e.g. 4 (beats per bar)
     * @param denominator  e.g. 4 (beat unit, must be a power of 2)
     */
    void setTimeSignature(uint16_t numerator, uint8_t denominator) noexcept;

    /**
     * @brief Set TPQN (Ticks Per Quarter Note).
     *
     * Default: 960 (kDefaultTPQN). Change this only if you have a compelling
     * reason — 960 is the industry standard (DAW-compatible).
     * All SequenceEvents added after this call use the new TPQN.
     */
    void setTPQN(uint16_t tpqn) noexcept { tpqn_ = tpqn; }

    /**
     * @brief Add UTF-8 key=value metadata to the MetaChunk.
     *
     * Example: addMeta("project", "My Beat Pack");
     * Keys and values must not contain '=' or newline characters.
     * Entries are written as "key=value\n" in the MetaChunk payload.
     */
    void addMeta(const std::string& key, const std::string& value);


    // ── Sample Accumulation ──────────────────────────────────────────────────

    /**
     * @brief Add a decoded audio sample to the bank.
     *
     * @param buffer  Float PCM, interleaved or planar (JUCE uses planar).
     *                The writer will convert to the target bit_depth.
     * @param meta    Metadata (sample rate, channels, bit depth, etc.)
     * @param[out] assignedID  If non-null, receives the SampleID assigned
     *                         to this sample (0-based, insertion order).
     * @return Error::None on success, or an error code.
     *
     * IMPORTANT: The `channels` field in `meta` must match
     * `buffer.getNumChannels()`. If they differ, the call fails with
     * Error::InvalidChannelCount.
     *
     * ZERO-CROSSING TRIM: This method automatically trims the end of the
     * buffer to the nearest zero-crossing to prevent click artefacts on
     * loop playback. The original buffer is NOT modified; trimming affects
     * only what gets written to the file.
     */
    [[nodiscard]] Error addSample(const juce::AudioBuffer<float>& buffer,
                                  const SampleMeta&               meta,
                                  uint16_t*                       assignedID = nullptr);


    // ── Event Accumulation ───────────────────────────────────────────────────

    /**
     * @brief Add a pre-built SequenceEvent to the sequence.
     *
     * Use `SequenceEvent::set()` to construct the event before calling this.
     * Events may be added in any order — write() sorts them by tick_start.
     *
     * @return Error::None on success, or Error::TooManyEvents.
     */
    [[nodiscard]] Error addEvent(const SequenceEvent& event);

    /**
     * @brief Convenience overload: construct and add a SequenceEvent inline.
     */
    [[nodiscard]] Error addEvent(uint32_t tickStart,
                                 uint16_t sampleID,
                                 uint8_t  velocity,
                                 uint32_t durationTicks,
                                 bool     oneShot = false);


    // ── Compilation ──────────────────────────────────────────────────────────

    /**
     * @brief Serialise all accumulated data to a `.LOOP` binary file.
     *
     * This is the "compiler" function. Steps performed:
     *   1. Validate all accumulated state (sample counts, event ranges, etc.)
     *   2. Sort SequenceEvents by tick_start (required for binary-search in player)
     *   3. Write FileHeader with placeholder offsets
     *   4. Write SampleLUT entries
     *   5. Write each SampleDataBlock header + PCM payload (with CRC32)
     *   6. Write SequenceHeader + SequenceEvent array (with CRC32)
     *   7. Write MetaChunk (if any meta was added)
     *   8. Seek back to FileHeader → patch all offsets
     *   9. Compute FileHeader CRC32 and write it
     *
     * The write is performed to a temporary file first, then renamed to
     * `outputPath` atomically — partial writes on error leave the original
     * file intact.
     *
     * @param outputPath  Target file path (created or overwritten).
     * @return Error::None on success, or an error code on failure.
     *
     * NOTE: This method is synchronous and may block the calling thread for
     * several seconds with large sample banks. Call from a background thread
     * in the GUI (use juce::ThreadPool or std::async).
     */
    [[nodiscard]] Error write(const juce::File& outputPath);

    /**
     * @brief Reset all internal state. Safe to reuse the writer after this.
     */
    void clear() noexcept;


    // ── Introspection (for UI display / progress reporting) ─────────────────

    [[nodiscard]] size_t getSampleCount() const noexcept { return samples_.size(); }
    [[nodiscard]] size_t getEventCount()  const noexcept { return events_.size();  }

    /// Returns the total byte size the output file would be, if written now.
    /// Useful for showing a file-size estimate in the UI before writing.
    [[nodiscard]] uint64_t estimateOutputBytes() const noexcept;


private:
    // ─────────────────────────────────────────────────────────────────────────
    //  Internal types
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Internal representation of one sample entry before serialisation.
     * Holds the converted (integer) PCM and the LUT metadata.
     *
     * WHY store PCM as a byte vector here?
     *   We need to be able to CRC32 it and know its exact byte_length before
     *   we can write the SampleDataBlock header. Storing it here avoids a
     *   two-pass write.
     */
    struct PendingSample
    {
        SampleLUTEntry      lutEntry;   ///< Will be written verbatim to the LUT
        std::vector<uint8_t> pcmBytes;  ///< Converted integer PCM (packed 16 or 24-bit)
        std::string         name;       ///< Optional; emitted to MetaChunk
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Private helpers
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Convert a JUCE float AudioBuffer to packed integer PCM bytes.
     *
     * @param buffer     Source float buffer (planar, JUCE layout)
     * @param channels   Number of channels to interleave
     * @param frameCount Number of frames to convert (after ZC trim)
     * @param bitDepth   16 or 24
     * @param[out] out   Destination byte vector (resized to exact size)
     *
     * Float-to-int conversion:
     *   16-bit: sample_int16 = clamp(float × 32767.f, -32768, 32767)
     *   24-bit: sample_int32 = clamp(float × 8388607.f, -8388608, 8388607)
     *           → stored as 3 bytes, LE, sign-extended
     */
    static void convertFloatToInt(const juce::AudioBuffer<float>& buffer,
                                  int                             channels,
                                  int                             frameCount,
                                  uint8_t                         bitDepth,
                                  std::vector<uint8_t>&           out);

    /**
     * Find the zero-crossing trim point for a float buffer.
     * Wraps FormatSpec.h::findZeroCrossing_16 by temporarily converting the
     * last `kZCLookbackFrames` frames.
     *
     * Returns the trimmed frame count (≤ buffer.getNumSamples()).
     */
    static int findZeroCrossingTrim(const juce::AudioBuffer<float>& buffer,
                                    int channels) noexcept;

    /**
     * Write a single PendingSample (header + PCM) to the stream.
     * Updates `lutEntry.file_offset` to the position of the SampleDataBlock.
     *
     * @return false if the write fails (disk full, etc.)
     */
    static bool writeSampleBlock(juce::FileOutputStream& stream,
                                 PendingSample&          sample);

    /**
     * Validate that a newly-added sample is consistent with already-added ones.
     * Specifically: all samples must share the same sample_rate.
     */
    [[nodiscard]] Error validateSampleMeta(const SampleMeta& meta) const noexcept;

    /**
     * Returns the total byte size of all SampleDataBlock headers + PCM payloads.
     * Used internally by write() to seek past the sample data region after
     * re-writing the LUT with patched file_offsets.
     */
    [[nodiscard]] uint64_t estimateSampleDataBytes() const noexcept;

    // ─────────────────────────────────────────────────────────────────────────
    //  State
    // ─────────────────────────────────────────────────────────────────────────

    std::vector<PendingSample>                  samples_;
    std::vector<SequenceEvent>                  events_;
    std::vector<std::pair<std::string,std::string>> metaEntries_;

    float    masterBpm_   { 120.f };
    uint16_t timeSigNum_  { 4 };
    uint8_t  timeSigDen_  { 4 };
    uint16_t tpqn_        { kDefaultTPQN };

    /// Cached sample rate from the first addSample() call.
    /// Subsequent calls must match this value.
    std::optional<uint32_t> lockedSampleRate_;

    // ZC look-back window: how many frames to scan backwards from loop end.
    // 0.5 seconds at 44100 Hz = 22050 frames. Adjust if needed.
    static constexpr int kZCLookbackFrames = 22050;
};

} // namespace LoopFormat
