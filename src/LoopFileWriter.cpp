/**
 * @file    LoopFileWriter.cpp
 * @brief   Implementation of LoopFileWriter — the Creator Tool compiler.
 *
 * =============================================================================
 * AGENT / DEVELOPER NOTES
 * =============================================================================
 *
 * READ THIS BEFORE EDITING
 * ------------------------
 * The most important invariant this file must maintain:
 *
 *   SequenceEvent[] in the output file is ALWAYS sorted ascending
 *   by tick_start. If you add any code path that skips the sort,
 *   the binary-search in LoopSequencer::getNextAudioBlock() will
 *   silently miss events and produce wrong playback.
 *
 * THE TWO-PASS WRITE PATTERN
 * --------------------------
 * We cannot know file offsets until we've written all the data. So:
 *   Pass 1: Write FileHeader with ZERO offsets as a placeholder.
 *   Pass 2: After all data is written, seek back to offset 0 and
 *           overwrite the FileHeader with the real offsets + CRC32.
 *
 * This is standard practice for binary container formats (RIFF, IFF, etc.)
 *
 * PCM BYTE LAYOUT (interleaved)
 * ------------------------------
 * For a stereo 16-bit buffer with frames [F0, F1, F2]:
 *   Bytes: [F0_L_lo, F0_L_hi, F0_R_lo, F0_R_hi,
 *           F1_L_lo, F1_L_hi, F1_R_lo, F1_R_hi, ...]
 * For 24-bit:
 *   Bytes: [F0_L_b0, F0_L_b1, F0_L_b2,   <- LE sign-magnitude
 *           F0_R_b0, F0_R_b1, F0_R_b2, ...]
 *
 * The reader (LoopFileReader) uses bytesPerFrame() from the LUT entry,
 * so the exact layout is implicitly encoded in channels + bit_depth.
 *
 * ATOMIC WRITE STRATEGY
 * ----------------------
 * write() writes to a temp file ("<outputPath>.tmp") first, then calls
 * juce::File::moveFileTo() to atomically rename it. This ensures the
 * original .LOOP file is never left in a partially-written state on error
 * or crash.
 *
 * =============================================================================
 */

#include "../include/LoopFileWriter.h"

#include <algorithm>   // std::sort, std::clamp
#include <cassert>
#include <cstring>     // std::memset, std::memcpy
#include <numeric>     // std::accumulate

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Static helpers (file-local)
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
    /**
     * Clamp a float in [-1.f, 1.f] and scale to a 16-bit integer.
     * Uses truncation-toward-zero (consistent, no dithering here).
     *
     * Dithering note: For a production tool you may want to add TPDF dither
     * before truncation when down-converting from float. Not implemented here
     * to keep the core simple — add a Ditherer class if needed.
     */
    [[nodiscard]] LOOP_FORCE_INLINE
    int16_t floatToInt16(float s) noexcept
    {
        s = std::clamp(s, -1.f, 1.f);
        return static_cast<int16_t>(s * 32767.f);
    }

    /**
     * Clamp and scale to 24-bit (stored as a 32-bit int, upper byte unused).
     */
    [[nodiscard]] LOOP_FORCE_INLINE
    int32_t floatToInt24(float s) noexcept
    {
        s = std::clamp(s, -1.f, 1.f);
        return static_cast<int32_t>(s * 8388607.f);
    }

    /**
     * Write a 24-bit integer as 3 bytes, little-endian, into dst.
     * dst must have at least 3 bytes of space.
     */
    LOOP_FORCE_INLINE
    void writeInt24LE(uint8_t* dst, int32_t value) noexcept
    {
        dst[0] = static_cast<uint8_t>( value        & 0xFF);
        dst[1] = static_cast<uint8_t>((value >> 8)  & 0xFF);
        dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    }

    /**
     * Write a FileOutputStream and abort with an assertion in Debug
     * if the write fails (disk full / permissions). In Release, the
     * outer write() function checks the stream's status after each block.
     */
    LOOP_FORCE_INLINE
    bool streamWrite(juce::FileOutputStream& s, const void* data, size_t bytes)
    {
        const bool ok = s.write(data, bytes);
        assert(ok && "FileOutputStream::write() failed — disk full?");
        return ok;
    }

} // anonymous namespace


// ─────────────────────────────────────────────────────────────────────────────
//  Error description
// ─────────────────────────────────────────────────────────────────────────────

const char* LoopFileWriter::describeError(Error e) noexcept
{
    switch (e)
    {
        case Error::None:                return "Success";
        case Error::TooManySamples:      return "Too many samples (max 4096)";
        case Error::TooManyEvents:       return "Too many events (max ~1M)";
        case Error::InvalidBitDepth:     return "Bit depth must be 16 or 24";
        case Error::InvalidChannelCount: return "Channel count must be 1 or 2";
        case Error::SampleRateMismatch:  return "All samples must share one sample rate";
        case Error::BufferEmpty:         return "Audio buffer has zero frames";
        case Error::FileOpenFailed:      return "Could not open output file for writing";
        case Error::WriteError:          return "Write failed (disk full or permissions)";
        case Error::NoSamples:           return "No samples added before write()";
        case Error::InternalError:       return "Internal error (bug — please report)";
        default:                         return "Unknown error";
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────────────────────────────────────────

void LoopFileWriter::setTimeSignature(uint16_t numerator,
                                      uint8_t  denominator) noexcept
{
    // denominator must be a power of 2 (1, 2, 4, 8, 16...)
    assert(denominator >= 1 && (denominator & (denominator - 1)) == 0);
    timeSigNum_ = numerator;
    timeSigDen_ = denominator;
}

void LoopFileWriter::addMeta(const std::string& key, const std::string& value)
{
    // Disallow '=' and '\n' in keys/values — they are our delimiters.
    assert(key.find('=')  == std::string::npos);
    assert(key.find('\n') == std::string::npos);
    assert(value.find('\n') == std::string::npos);
    metaEntries_.emplace_back(key, value);
}


// ─────────────────────────────────────────────────────────────────────────────
//  Sample accumulation
// ─────────────────────────────────────────────────────────────────────────────

LoopFileWriter::Error
LoopFileWriter::addSample(const juce::AudioBuffer<float>& buffer,
                          const SampleMeta&               meta,
                          uint16_t*                       assignedID)
{
    // ── 1. Guard checks ──────────────────────────────────────────────────────
    if (samples_.size() >= kMaxSamples)
        return Error::TooManySamples;

    if (meta.bit_depth != 16 && meta.bit_depth != 24)
        return Error::InvalidBitDepth;

    if (meta.channels < 1 || meta.channels > 2)
        return Error::InvalidChannelCount;

    if (buffer.getNumSamples() == 0)
        return Error::BufferEmpty;

    // The channel count in meta must match what the buffer actually has.
    // (juce::AudioBuffer stores one array per channel — it's planar, not interleaved)
    if (buffer.getNumChannels() < static_cast<int>(meta.channels))
        return Error::InvalidChannelCount;

    if (auto err = validateSampleMeta(meta); err != Error::None)
        return err;

    // ── 2. Zero-crossing trim ────────────────────────────────────────────────
    //
    // findZeroCrossingTrim() scans BACKWARDS from the end of the buffer
    // to find the last frame where all channels are near 0.
    // We pass this trimmed frame count into convertFloatToInt() so we
    // never encode the "clickable" tail.
    //
    const int naturalFrameCount = buffer.getNumSamples();
    const int trimmedFrameCount = findZeroCrossingTrim(buffer,
                                                        meta.channels);

    // ── 3. Float → integer PCM conversion ───────────────────────────────────
    PendingSample pending;
    convertFloatToInt(buffer,
                      meta.channels,
                      trimmedFrameCount,
                      meta.bit_depth,
                      pending.pcmBytes);

    // ── 4. Fill LUT entry ────────────────────────────────────────────────────
    const auto id = static_cast<uint16_t>(samples_.size());

    SampleLUTEntry& lut = pending.lutEntry;
    lut.sample_id   = id;
    lut.channels    = meta.channels;
    lut.sample_rate = meta.sample_rate;
    lut.frame_count = static_cast<uint32_t>(trimmedFrameCount);
    lut.byte_length = static_cast<uint32_t>(pending.pcmBytes.size());
    lut.base_note   = meta.base_note;
    lut.loop_bpm    = meta.loop_bpm;
    lut.file_offset = 0; // Patched during write()

    // Compose flags
    lut.flags  = (meta.bit_depth == 16) ? kBitDepth16 : kBitDepth24;
    lut.flags |= (meta.channels  == 2)  ? kFlagStereo : 0;
    lut.flags |= kFlagLoopable; // ZC trim was applied

    pending.name = meta.name;

    // Log trim for debugging (no-op in release)
    if (trimmedFrameCount < naturalFrameCount)
    {
        const int trimmedFrames = naturalFrameCount - trimmedFrameCount;
        // juce::Logger::writeToLog(...) would go here in a full app
        (void)trimmedFrames;
    }

    // ── 5. Commit ────────────────────────────────────────────────────────────
    if (assignedID) *assignedID = id;
    samples_.push_back(std::move(pending));
    return Error::None;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Event accumulation
// ─────────────────────────────────────────────────────────────────────────────

LoopFileWriter::Error LoopFileWriter::addEvent(const SequenceEvent& event)
{
    if (events_.size() >= kMaxEvents)
        return Error::TooManyEvents;

    events_.push_back(event);
    return Error::None;
}

LoopFileWriter::Error LoopFileWriter::addEvent(uint32_t tickStart,
                                               uint16_t sampleID,
                                               uint8_t  velocity,
                                               uint32_t durationTicks,
                                               bool     oneShot)
{
    SequenceEvent evt;
    evt.set(tickStart, sampleID, velocity, durationTicks, oneShot);
    return addEvent(evt);
}


// ─────────────────────────────────────────────────────────────────────────────
//  Compilation — write() — the main serialiser
// ─────────────────────────────────────────────────────────────────────────────

LoopFileWriter::Error LoopFileWriter::write(const juce::File& outputPath)
{
    // ── Precondition checks ──────────────────────────────────────────────────
    if (samples_.empty())
        return Error::NoSamples;

    // Sort events by tick_start (ascending).
    // This is MANDATORY — LoopSequencer uses std::lower_bound on this array.
    std::sort(events_.begin(), events_.end(),
              [](const SequenceEvent& a, const SequenceEvent& b) {
                  return a.tickStart() < b.tickStart();
              });

    // ── Open a temp file for atomic write ────────────────────────────────────
    //
    // We write to "<path>.tmp" and rename on success. This guarantees the
    // destination file is never in a partial state if we crash mid-write.
    //
    const juce::File tempFile = outputPath.getSiblingFile(
        outputPath.getFileName() + ".tmp");

    tempFile.deleteFile(); // Remove any stale temp from a previous failed run

    auto stream = std::make_unique<juce::FileOutputStream>(tempFile);
    if (stream->failedToOpen())
        return Error::FileOpenFailed;

    stream->setPosition(0);
    stream->truncate();

    // ── PASS 1: Write placeholder FileHeader ─────────────────────────────────
    //
    // We write a zeroed-out header first to reserve the 64 bytes at offset 0.
    // After all data is written, we seek back here and overwrite it with the
    // real offsets and CRC32.
    //
    FileHeader header;
    header.init();
    header.sample_count = static_cast<uint32_t>(samples_.size());
    header.event_count  = static_cast<uint32_t>(events_.size());
    header.tpqn         = tpqn_;
    // All offsets left as 0 for now — patched in Pass 2.

    if (!streamWrite(*stream, &header, sizeof(FileHeader)))
        return Error::WriteError;

    // ── Write SampleLUT ───────────────────────────────────────────────────────
    //
    // The LUT immediately follows the FileHeader. We record its offset
    // for the header patch, then write N × 32-byte SampleLUTEntry structs.
    //
    const uint64_t lutOffset = static_cast<uint64_t>(stream->getPosition());

    for (auto& s : samples_)
    {
        if (!streamWrite(*stream, &s.lutEntry, sizeof(SampleLUTEntry)))
            return Error::WriteError;
    }

    // ── Write Sample Data Blocks ──────────────────────────────────────────────
    //
    // Each block is: [SampleDataBlock header (16 bytes)] + [raw PCM bytes]
    // We patch each LUT entry's file_offset to point at its SampleDataBlock.
    // After all blocks are written, we re-write the LUT with updated offsets.
    //
    const uint64_t sampleDataOffset = static_cast<uint64_t>(stream->getPosition());

    for (auto& s : samples_)
    {
        // Patch the LUT entry's file_offset BEFORE writing the data block
        s.lutEntry.file_offset = static_cast<uint64_t>(stream->getPosition());

        if (!writeSampleBlock(*stream, s))
            return Error::WriteError;
    }

    // ── Re-write the LUT with patched file_offsets ───────────────────────────
    //
    // We now know every sample's absolute file offset. Seek back to the LUT
    // region and overwrite it.
    //
    stream->setPosition(static_cast<int64_t>(lutOffset));
    for (const auto& s : samples_)
    {
        if (!streamWrite(*stream, &s.lutEntry, sizeof(SampleLUTEntry)))
            return Error::WriteError;
    }
    // Seek back to end (after all sample data) to continue writing
    stream->setPosition(static_cast<int64_t>(
        sampleDataOffset + estimateSampleDataBytes()));

    // ── Write Sequence ────────────────────────────────────────────────────────
    const uint64_t sequenceOffset = static_cast<uint64_t>(stream->getPosition());

    // Compute total sequence duration (tick of last event + its duration)
    uint32_t totalDurationTicks = 0;
    for (const auto& evt : events_)
    {
        const uint32_t end = evt.tickStart() + evt.loopDuration();
        if (end > totalDurationTicks) totalDurationTicks = end;
    }

    // Build and write SequenceHeader
    SequenceHeader seqHeader;
    std::memset(&seqHeader, 0, sizeof(seqHeader));
    seqHeader.tag                  = kTagSequence;
    seqHeader.event_count          = static_cast<uint32_t>(events_.size());
    seqHeader.total_duration_ticks = totalDurationTicks;
    seqHeader.time_sig_numerator   = timeSigNum_;
    seqHeader.time_sig_denominator = timeSigDen_;
    seqHeader.bpm                  = masterBpm_;

    // CRC32 over the event array
    if (!events_.empty())
    {
        seqHeader.crc32_events = crc32(events_.data(),
                                       events_.size() * sizeof(SequenceEvent));
    }

    if (!streamWrite(*stream, &seqHeader, sizeof(SequenceHeader)))
        return Error::WriteError;

    // Write events
    if (!events_.empty())
    {
        if (!streamWrite(*stream, events_.data(),
                         events_.size() * sizeof(SequenceEvent)))
            return Error::WriteError;
    }

    // ── Write MetaChunk (optional) ────────────────────────────────────────────
    uint64_t metaOffset = 0;

    if (!metaEntries_.empty())
    {
        metaOffset = static_cast<uint64_t>(stream->getPosition());

        // Build UTF-8 payload: "key=value\n" per entry
        std::string payload;
        for (const auto& [k, v] : metaEntries_)
            payload += k + "=" + v + "\n";

        MetaChunkHeader metaHeader;
        std::memset(&metaHeader, 0, sizeof(metaHeader));
        metaHeader.tag              = kTagMeta;
        metaHeader.meta_byte_length = static_cast<uint32_t>(payload.size());
        metaHeader.entry_count      = static_cast<uint32_t>(metaEntries_.size());

        if (!streamWrite(*stream, &metaHeader, sizeof(MetaChunkHeader)))
            return Error::WriteError;

        if (!streamWrite(*stream, payload.data(), payload.size()))
            return Error::WriteError;

        // Append sample names as meta entries "s0.name=kick", etc.
        // (already included in metaEntries_ via addMeta during addSample)
    }

    // ── PASS 2: Patch FileHeader with real offsets + CRC32 ───────────────────
    //
    // Seek back to offset 0 and overwrite the FileHeader.
    // The CRC32 covers bytes [0..51] of the final header (excludes the
    // crc32_header field itself at bytes [52..55]).
    //
    header.lut_offset          = lutOffset;
    header.sample_data_offset  = sampleDataOffset;
    header.sequence_offset     = sequenceOffset;
    header.meta_offset         = metaOffset; // 0 if no meta

    // Zero the CRC field before computing it (it must be 0 during calculation)
    header.crc32_header = 0;
    header.crc32_header = crc32(&header, 52); // bytes 0..51 only

    stream->setPosition(0);
    if (!streamWrite(*stream, &header, sizeof(FileHeader)))
        return Error::WriteError;

    // Flush to disk
    stream->flush();

    // Verify stream is healthy
    if (stream->getStatus().failed())
        return Error::WriteError;

    // ── Atomic rename ─────────────────────────────────────────────────────────
    stream.reset(); // Close file before rename

    outputPath.deleteFile();
    if (!tempFile.moveFileTo(outputPath))
    {
        tempFile.deleteFile();
        return Error::WriteError;
    }

    return Error::None;
}


// ─────────────────────────────────────────────────────────────────────────────
//  clear()
// ─────────────────────────────────────────────────────────────────────────────

void LoopFileWriter::clear() noexcept
{
    samples_.clear();
    events_.clear();
    metaEntries_.clear();
    lockedSampleRate_.reset();
    masterBpm_  = 120.f;
    timeSigNum_ = 4;
    timeSigDen_ = 4;
    tpqn_       = kDefaultTPQN;
}


// ─────────────────────────────────────────────────────────────────────────────
//  estimateOutputBytes()
// ─────────────────────────────────────────────────────────────────────────────

uint64_t LoopFileWriter::estimateOutputBytes() const noexcept
{
    uint64_t total = sizeof(FileHeader);

    // LUT
    total += samples_.size() * sizeof(SampleLUTEntry);

    // Sample data blocks
    for (const auto& s : samples_)
        total += sizeof(SampleDataBlock) + s.pcmBytes.size();

    // Sequence
    total += sizeof(SequenceHeader);
    total += events_.size() * sizeof(SequenceEvent);

    // Meta
    for (const auto& [k, v] : metaEntries_)
        total += k.size() + 1 + v.size() + 1; // "key=value\n"
    if (!metaEntries_.empty())
        total += sizeof(MetaChunkHeader);

    return total;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Private helpers
// ─────────────────────────────────────────────────────────────────────────────

void LoopFileWriter::convertFloatToInt(const juce::AudioBuffer<float>& buffer,
                                        int                             channels,
                                        int                             frameCount,
                                        uint8_t                         bitDepth,
                                        std::vector<uint8_t>&           out)
{
    // Calculate exact output size
    const size_t bytesPerSample = (bitDepth == 24) ? 3 : 2;
    const size_t totalBytes = static_cast<size_t>(frameCount)
                            * static_cast<size_t>(channels)
                            * bytesPerSample;
    out.resize(totalBytes);

    uint8_t* dst = out.data();

    if (bitDepth == 16)
    {
        // Interleave: for each frame, write all channels consecutively
        for (int f = 0; f < frameCount; ++f)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float   s   = buffer.getSample(ch, f);
                const int16_t s16 = floatToInt16(s);

                // Little-endian 16-bit
                dst[0] = static_cast<uint8_t>( s16        & 0xFF);
                dst[1] = static_cast<uint8_t>((s16 >> 8)  & 0xFF);
                dst += 2;
            }
        }
    }
    else // 24-bit
    {
        for (int f = 0; f < frameCount; ++f)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float   s   = buffer.getSample(ch, f);
                const int32_t s24 = floatToInt24(s);
                writeInt24LE(dst, s24);
                dst += 3;
            }
        }
    }
}


int LoopFileWriter::findZeroCrossingTrim(
    const juce::AudioBuffer<float>& buffer,
    int channels) noexcept
{
    const int totalFrames = buffer.getNumSamples();
    if (totalFrames == 0) return 0;

    // How far back to look (capped at the buffer length)
    const int lookback = std::min(totalFrames, kZCLookbackFrames);

    // Threshold below which a float sample counts as "zero" (≈ -74dBFS)
    constexpr float kThreshold = 64.f / 32768.f;

    // Scan backwards from the natural end
    for (int f = totalFrames; f > (totalFrames - lookback); --f)
    {
        bool allNearZero = true;
        for (int ch = 0; ch < channels; ++ch)
        {
            const float s = buffer.getSample(ch, f - 1);
            if (s > kThreshold || s < -kThreshold)
            {
                allNearZero = false;
                break;
            }
        }
        if (allNearZero)
            return f;
    }

    // No zero-crossing found in lookback window — use natural end.
    // This is not an error; some loops (e.g. sustain pads) may never
    // cross zero near their end.
    return totalFrames;
}


bool LoopFileWriter::writeSampleBlock(juce::FileOutputStream& stream,
                                       PendingSample&          sample)
{
    // Build the SampleDataBlock header
    SampleDataBlock blk;
    std::memset(&blk, 0, sizeof(blk));
    blk.tag         = kTagSampleData;
    blk.sample_id   = sample.lutEntry.sample_id;
    blk.byte_length = static_cast<uint32_t>(sample.pcmBytes.size());
    blk.crc32_pcm   = crc32(sample.pcmBytes.data(), sample.pcmBytes.size());

    if (!streamWrite(stream, &blk, sizeof(SampleDataBlock)))
        return false;

    if (!sample.pcmBytes.empty())
    {
        if (!streamWrite(stream, sample.pcmBytes.data(), sample.pcmBytes.size()))
            return false;
    }

    return true;
}


LoopFileWriter::Error
LoopFileWriter::validateSampleMeta(const SampleMeta& meta) const noexcept
{
    // All samples in a single .LOOP file must share one sample rate.
    // The player doesn't do per-voice sample-rate conversion.
    if (lockedSampleRate_.has_value())
    {
        if (*lockedSampleRate_ != meta.sample_rate)
            return Error::SampleRateMismatch;
    }
    else
    {
        // Lock in the sample rate on first addition.
        // (const_cast is intentional — this is a "lazy initialise" pattern)
        const_cast<std::optional<uint32_t>&>(lockedSampleRate_) = meta.sample_rate;
    }
    return Error::None;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Internal utility: estimate bytes for all sample data blocks
//  Used by write() when seeking to the correct position after re-writing LUT.
// ─────────────────────────────────────────────────────────────────────────────

uint64_t LoopFileWriter::estimateSampleDataBytes() const noexcept
{
    uint64_t total = 0;
    for (const auto& s : samples_)
        total += sizeof(SampleDataBlock) + s.pcmBytes.size();
    return total;
}

} // namespace LoopFormat
