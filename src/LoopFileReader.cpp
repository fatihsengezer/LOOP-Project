/**
 * @file    LoopFileReader.cpp
 * @brief   Implementation of LoopFileReader — memory-mapped `.LOOP` file reader.
 *
 * =============================================================================
 * AGENT / DEVELOPER NOTES
 * =============================================================================
 *
 * READ BEFORE EDITING
 * --------------------
 * This file's primary contract:
 *   After open() returns Error::None, the following must hold forever
 *   (until close() is called):
 *
 *   A. getSamplePointer(id) returns a non-null pointer for any id < sampleCount_
 *   B. getEventsBegin() / getEventsEnd() span exactly eventCount_ events
 *   C. lutCache_[id].file_offset points to a valid SampleDataBlock
 *
 * If any of these is violated, the audio thread will dereference a dangling or
 * out-of-bounds pointer, which is undefined behaviour with no recovery path.
 * THIS IS WHY open() does exhaustive validation before setting isValid_ = true.
 *
 * MMAP vs. READ
 * ──────────────
 * We use juce::MemoryMappedFile with accessPattern = sequential. This hint
 * tells the OS to pre-fetch pages ahead of the current read position, which
 * is optimal for playing a sequence that triggers samples roughly in order.
 * For random-access patterns (e.g. live performance jumping around the bank),
 * change to juce::MemoryMappedFile::AccessPattern::random.
 *
 * CRC VERIFICATION STRATEGY
 * ──────────────────────────
 * open() verifies:
 *   - FileHeader CRC32  (fast — 52 bytes)
 *   - Each SampleDataBlock's sample_id tag (fast — one uint16 per sample)
 *   - NOT the PCM payload CRC (that would read the entire file on open)
 *
 * PCM CRC is verified lazily via verifySampleCRC(id). Recommended pattern:
 *   1. open() succeeds → start playing immediately
 *   2. Launch a background thread → call verifyAllSampleCRCs()
 *   3. If CRC fails → show a warning in the UI, stop playback
 *
 * LUT CACHE DESIGN
 * ─────────────────
 * The mmap'd LUT region is packed (no alignment guarantees). Reading multi-byte
 * fields directly from a packed, potentially unaligned pointer causes undefined
 * behaviour on ARM and a performance penalty on x86. So buildLUTCache() copies
 * each 32-byte SampleLUTEntry into lutCache_ (a std::array with natural alignment).
 * The copy is done once in open(); the audio thread always reads from lutCache_.
 *
 * =============================================================================
 */

#include "../include/LoopFileReader.h"

#include <cstring>    // std::memcpy
#include <cassert>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Error description
// ─────────────────────────────────────────────────────────────────────────────

const char* LoopFileReader::describeError(Error e) noexcept
{
    switch (e)
    {
        case Error::None:                return "Success";
        case Error::FileNotFound:        return "File not found";
        case Error::MMapFailed:          return "Memory-map failed (file in use or no permission)";
        case Error::InvalidMagic:        return "Not a .LOOP file (invalid magic bytes)";
        case Error::IncompatibleVersion: return "Incompatible file version (need version 1.x)";
        case Error::HeaderCRCFailed:     return "File header is corrupted (CRC mismatch)";
        case Error::OffsetOutOfBounds:   return "Chunk offset points outside the file";
        case Error::LUTCRCFailed:        return "Sample lookup table is corrupted";
        case Error::SampleCRCFailed:     return "Sample PCM data is corrupted (CRC mismatch)";
        case Error::InvalidSampleID:     return "Sample ID is out of range";
        case Error::AlreadyOpen:         return "Reader already has a file open; call close() first";
        default:                         return "Unknown error";
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Move semantics
// ─────────────────────────────────────────────────────────────────────────────

LoopFileReader::LoopFileReader(LoopFileReader&& other) noexcept
    : mappedFile_     (std::move(other.mappedFile_))
    , lutCache_       (other.lutCache_)
    , metaMap_        (other.metaMap_)
    , sampleCount_    (other.sampleCount_)
    , eventCount_     (other.eventCount_)
    , sequenceOffset_ (other.sequenceOffset_)
    , isValid_        (other.isValid_.load(std::memory_order_relaxed))
{
    // Invalidate the moved-from object
    other.isValid_.store(false, std::memory_order_release);
    other.sampleCount_ = 0;
    other.eventCount_  = 0;
}

LoopFileReader& LoopFileReader::operator=(LoopFileReader&& other) noexcept
{
    if (this != &other)
    {
        close();
        mappedFile_     = std::move(other.mappedFile_);
        lutCache_       = other.lutCache_;
        metaMap_        = other.metaMap_;
        sampleCount_    = other.sampleCount_;
        eventCount_     = other.eventCount_;
        sequenceOffset_ = other.sequenceOffset_;
        isValid_.store(other.isValid_.load(std::memory_order_relaxed),
                       std::memory_order_release);

        other.isValid_.store(false, std::memory_order_release);
        other.sampleCount_ = 0;
        other.eventCount_  = 0;
    }
    return *this;
}


// ─────────────────────────────────────────────────────────────────────────────
//  open()
// ─────────────────────────────────────────────────────────────────────────────

LoopFileReader::Error LoopFileReader::open(const juce::File& file)
{
    // ── Guard ────────────────────────────────────────────────────────────────
    if (isValid_.load(std::memory_order_acquire))
        return Error::AlreadyOpen;

    if (!file.existsAsFile())
        return Error::FileNotFound;

    // ── Memory-map the file ───────────────────────────────────────────────────
    //
    // juce::MemoryMappedFile maps the entire file into the process address
    // space. No file data is read from disk at this point — the OS will load
    // 4KB pages on demand (page faults) as we access them.
    //
    // exclusiveAccessMode = false → allow other processes to read the file
    //                              (important for multi-instance plugin hosts)
    //
    mappedFile_ = std::make_unique<juce::MemoryMappedFile>(
        file,
        juce::MemoryMappedFile::readOnly,
        /*exclusiveAccessMode=*/ false);

    if (mappedFile_->getData() == nullptr)
    {
        mappedFile_.reset();
        return Error::MMapFailed;
    }

    // ── Read and validate FileHeader ──────────────────────────────────────────
    //
    // The header is always at offset 0. We use mappedPtr<FileHeader>(0) which
    // gives us a pointer into the mmap'd region.
    //
    // IMPORTANT: We immediately copy the header into a local variable.
    // Do NOT store a pointer to the mmap'd header and use it later —
    // if the file is modified on disk while mapped, the contents become
    // undefined. The local copy is immutable for the rest of open().
    //
    const FileHeader* rawHeader = mappedPtr<FileHeader>(0);
    if (!rawHeader)
    {
        close();
        return Error::OffsetOutOfBounds; // File is smaller than 64 bytes
    }

    // Copy to local aligned struct (avoids unaligned reads on ARM)
    FileHeader header;
    std::memcpy(&header, rawHeader, sizeof(FileHeader));

    // Check magic bytes
    if (!header.hasMagic())
    {
        close();
        return Error::InvalidMagic;
    }

    // Check version compatibility
    if (!isVersionCompatible(header.version_major, header.version_minor))
    {
        close();
        return Error::IncompatibleVersion;
    }

    // Validate header CRC32
    if (auto err = validateHeaderCRC(header); err != Error::None)
    {
        close();
        return err;
    }

    // ── Validate chunk offsets ────────────────────────────────────────────────
    //
    // Each offset must point to a region within the file. We check the
    // minimum required size for each chunk header (not the full payload,
    // which would require reading large amounts of data).
    //
    const uint64_t fileSize = static_cast<uint64_t>(mappedFile_->getSize());

    // LUT region
    const uint64_t lutBytes = static_cast<uint64_t>(header.sample_count)
                            * sizeof(SampleLUTEntry);
    if (!isOffsetValid(header.lut_offset, lutBytes))
    {
        close();
        return Error::OffsetOutOfBounds;
    }

    // Sequence header
    if (!isOffsetValid(header.sequence_offset, sizeof(SequenceHeader)))
    {
        close();
        return Error::OffsetOutOfBounds;
    }

    // Sequence events
    const uint64_t evtBytes = static_cast<uint64_t>(header.event_count)
                            * sizeof(SequenceEvent);
    if (!isOffsetValid(header.sequence_offset + sizeof(SequenceHeader), evtBytes))
    {
        close();
        return Error::OffsetOutOfBounds;
    }

    // Meta chunk (optional — 0 means absent)
    if (header.meta_offset != 0 &&
        !isOffsetValid(header.meta_offset, sizeof(MetaChunkHeader)))
    {
        close();
        return Error::OffsetOutOfBounds;
    }

    (void)fileSize; // suppress unused-variable warning in release

    // ── Cache scalar values ───────────────────────────────────────────────────
    sampleCount_    = header.sample_count;
    eventCount_     = header.event_count;
    sequenceOffset_ = header.sequence_offset;

    // ── Build LUT cache ───────────────────────────────────────────────────────
    //
    // Copies the packed mmap'd LUT entries into an aligned std::array.
    // This is the only place we read the LUT from the mapped file.
    //
    if (auto err = buildLUTCache(header); err != Error::None)
    {
        close();
        return err;
    }

    // ── Validate sample offsets ───────────────────────────────────────────────
    //
    // For each LUT entry, verify that file_offset → SampleDataBlock has
    // a matching sample_id and byte_length that fits within the file.
    //
    if (auto err = validateSampleOffsets(header); err != Error::None)
    {
        close();
        return err;
    }

    // ── Parse MetaChunk ───────────────────────────────────────────────────────
    if (header.meta_offset != 0)
        parseMetaChunk(header);

    // ── All checks passed — mark as valid ────────────────────────────────────
    //
    // isValid_ is the "gate" that audio-thread callers check.
    // We use memory_order_release so all preceding writes are visible
    // to any thread that subsequently reads isValid_ with memory_order_acquire.
    //
    isValid_.store(true, std::memory_order_release);
    return Error::None;
}


// ─────────────────────────────────────────────────────────────────────────────
//  close()
// ─────────────────────────────────────────────────────────────────────────────

void LoopFileReader::close() noexcept
{
    // Signal invalidity before destroying the map
    isValid_.store(false, std::memory_order_release);

    mappedFile_.reset();

    sampleCount_    = 0;
    eventCount_     = 0;
    sequenceOffset_ = 0;

    metaMap_.clear();
    // lutCache_ values left stale — guarded by isValid_ = false
}


// ─────────────────────────────────────────────────────────────────────────────
//  Header / sequence access
// ─────────────────────────────────────────────────────────────────────────────

FileHeader LoopFileReader::getFileHeader() const noexcept
{
    FileHeader h {};
    const auto* raw = mappedPtr<FileHeader>(0);
    if (raw) std::memcpy(&h, raw, sizeof(FileHeader));
    return h;
}

SequenceHeader LoopFileReader::getSequenceHeader() const noexcept
{
    SequenceHeader h {};
    const auto* raw = mappedPtr<SequenceHeader>(sequenceOffset_);
    if (raw) std::memcpy(&h, raw, sizeof(SequenceHeader));
    return h;
}


// ─────────────────────────────────────────────────────────────────────────────
//  LUT access (audio-thread hot path)
// ─────────────────────────────────────────────────────────────────────────────

const SampleLUTEntry& LoopFileReader::getLUTEntry(uint16_t sampleID) const noexcept
{
    assert(sampleID < sampleCount_ && "SampleID out of range");
    // Direct array access — O(1), no branch, no lock.
    return lutCache_[sampleID];
}


// ─────────────────────────────────────────────────────────────────────────────
//  PCM pointer (audio-thread hot path)
// ─────────────────────────────────────────────────────────────────────────────

const uint8_t* LoopFileReader::getSamplePointer(uint16_t sampleID) const noexcept
{
    if (!isValid_.load(std::memory_order_acquire)) return nullptr;
    if (sampleID >= sampleCount_)                  return nullptr;

    const SampleLUTEntry& lut = lutCache_[sampleID];

    // The LUT entry's file_offset points at the SampleDataBlock header.
    // The PCM bytes start immediately after that header.
    const uint64_t pcmOffset = lut.file_offset + sizeof(SampleDataBlock);

    const auto* base = static_cast<const uint8_t*>(mappedFile_->getData());
    const uint64_t fileSize = static_cast<uint64_t>(mappedFile_->getSize());

    if (pcmOffset + lut.byte_length > fileSize) return nullptr;

    return base + pcmOffset;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Sequence event access (audio-thread hot path)
// ─────────────────────────────────────────────────────────────────────────────

const SequenceEvent* LoopFileReader::getEventsBegin() const noexcept
{
    if (!isValid_.load(std::memory_order_acquire)) return nullptr;
    // Events start after the SequenceHeader
    return mappedPtr<SequenceEvent>(sequenceOffset_ + sizeof(SequenceHeader));
}

const SequenceEvent* LoopFileReader::getEventsEnd() const noexcept
{
    const SequenceEvent* begin = getEventsBegin();
    if (!begin) return nullptr;
    return begin + eventCount_;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Lazy CRC verification (background thread)
// ─────────────────────────────────────────────────────────────────────────────

LoopFileReader::Error LoopFileReader::verifySampleCRC(uint16_t sampleID) const noexcept
{
    if (!isValid_.load(std::memory_order_acquire)) return Error::MMapFailed;
    if (sampleID >= sampleCount_)                  return Error::InvalidSampleID;

    const SampleLUTEntry& lut = lutCache_[sampleID];

    // Read the SampleDataBlock header to get the stored CRC
    const auto* blk = mappedPtr<SampleDataBlock>(lut.file_offset);
    if (!blk) return Error::OffsetOutOfBounds;

    SampleDataBlock blkCopy;
    std::memcpy(&blkCopy, blk, sizeof(SampleDataBlock));

    // Compute CRC over the PCM payload
    const uint8_t* pcm = getSamplePointer(sampleID);
    if (!pcm) return Error::OffsetOutOfBounds;

    const uint32_t computed = crc32(pcm, blkCopy.byte_length);

    if (computed != blkCopy.crc32_pcm)
        return Error::SampleCRCFailed;

    return Error::None;
}

LoopFileReader::Error
LoopFileReader::verifyAllSampleCRCs(uint16_t* failedID) const noexcept
{
    for (uint16_t id = 0; id < static_cast<uint16_t>(sampleCount_); ++id)
    {
        const Error err = verifySampleCRC(id);
        if (err != Error::None)
        {
            if (failedID) *failedID = id;
            return err;
        }
    }
    return Error::None;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Meta access
// ─────────────────────────────────────────────────────────────────────────────

std::string LoopFileReader::getMetaValue(const std::string& key) const
{
    const juce::String juceKey (key.c_str());
    return metaMap_.getValue(juceKey, "").toStdString();
}


// ─────────────────────────────────────────────────────────────────────────────
//  Private helpers
// ─────────────────────────────────────────────────────────────────────────────

bool LoopFileReader::isOffsetValid(uint64_t offset, uint64_t size) const noexcept
{
    if (!mappedFile_ || !mappedFile_->getData()) return false;
    const uint64_t fileSize = static_cast<uint64_t>(mappedFile_->getSize());
    // Guard against overflow: offset + size could wrap around
    if (offset > fileSize)            return false;
    if (size   > fileSize - offset)   return false;
    return true;
}


LoopFileReader::Error
LoopFileReader::validateHeaderCRC(const FileHeader& h) const noexcept
{
    // CRC32 covers bytes [0..51] of the FileHeader.
    // The crc32_header field itself lives at bytes [52..55] and must be
    // excluded from the computation (it's zeroed before computing on write).
    //
    // We build a local copy with the CRC field zeroed, then compute.
    FileHeader check;
    std::memcpy(&check, &h, sizeof(FileHeader));
    check.crc32_header = 0;

    const uint32_t computed = crc32(&check, 52); // bytes 0..51 only

    if (computed != h.crc32_header)
        return Error::HeaderCRCFailed;

    return Error::None;
}


LoopFileReader::Error LoopFileReader::buildLUTCache(const FileHeader& h)
{
    // The LUT is a contiguous array of packed SampleLUTEntry structs.
    // We copy them one by one into the aligned lutCache_ array.
    //
    // std::memcpy is used (rather than dereferencing the packed pointer)
    // to avoid UB from unaligned access. Each entry is exactly 32 bytes.

    for (uint32_t i = 0; i < h.sample_count; ++i)
    {
        const uint64_t entryOffset = h.lut_offset
                                   + static_cast<uint64_t>(i) * sizeof(SampleLUTEntry);

        const auto* raw = mappedPtr<SampleLUTEntry>(entryOffset);
        if (!raw) return Error::OffsetOutOfBounds;

        std::memcpy(&lutCache_[i], raw, sizeof(SampleLUTEntry));

        // Basic sanity: sample_id in the LUT must match its index
        if (lutCache_[i].sample_id != static_cast<uint16_t>(i))
            return Error::LUTCRCFailed; // Mismatched ID = corrupted LUT
    }

    return Error::None;
}


LoopFileReader::Error
LoopFileReader::validateSampleOffsets(const FileHeader& h) const noexcept
{
    for (uint32_t i = 0; i < h.sample_count; ++i)
    {
        const SampleLUTEntry& lut = lutCache_[i];

        // Verify the SampleDataBlock header at the stored offset
        const auto* blk = mappedPtr<SampleDataBlock>(lut.file_offset);
        if (!blk) return Error::OffsetOutOfBounds;

        // Copy to avoid unaligned reads
        SampleDataBlock blkCopy;
        std::memcpy(&blkCopy, blk, sizeof(SampleDataBlock));

        // Verify FourCC tag
        if (blkCopy.tag != kTagSampleData) return Error::OffsetOutOfBounds;

        // Verify sample_id matches
        if (blkCopy.sample_id != lut.sample_id) return Error::LUTCRCFailed;

        // Verify the PCM payload fits within the file
        const uint64_t pcmOffset = lut.file_offset + sizeof(SampleDataBlock);
        if (!isOffsetValid(pcmOffset, blkCopy.byte_length))
            return Error::OffsetOutOfBounds;
    }

    return Error::None;
}


void LoopFileReader::parseMetaChunk(const FileHeader& h)
{
    if (h.meta_offset == 0) return;

    const auto* rawHeader = mappedPtr<MetaChunkHeader>(h.meta_offset);
    if (!rawHeader) return;

    MetaChunkHeader metaHdr;
    std::memcpy(&metaHdr, rawHeader, sizeof(MetaChunkHeader));

    if (metaHdr.tag != kTagMeta || metaHdr.meta_byte_length == 0)
        return;

    // Validate payload offset
    const uint64_t payloadOffset = h.meta_offset + sizeof(MetaChunkHeader);
    if (!isOffsetValid(payloadOffset, metaHdr.meta_byte_length)) return;

    // Read the UTF-8 payload
    const auto* payloadBytes = static_cast<const char*>(mappedFile_->getData())
                             + payloadOffset;

    const juce::String payload (payloadBytes,
                                static_cast<size_t>(metaHdr.meta_byte_length));

    // Parse "key=value\n" lines
    juce::StringArray lines;
    lines.addTokens(payload, "\n", "");

    for (const auto& line : lines)
    {
        if (line.isEmpty()) continue;
        const int eqPos = line.indexOfChar('=');
        if (eqPos < 1) continue; // malformed — skip

        const juce::String key   = line.substring(0, eqPos).trim();
        const juce::String value = line.substring(eqPos + 1);
        if (key.isNotEmpty())
            metaMap_.set(key, value);
    }
}

} // namespace LoopFormat
