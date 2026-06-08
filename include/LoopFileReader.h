/**
 * @file    LoopFileReader.h
 * @brief   Player-side memory-mapped `.LOOP` file reader.
 *
 * =============================================================================
 * AGENT / DEVELOPER QUICK-START
 * =============================================================================
 *
 * PURPOSE
 *   This class is used exclusively by the Player (LoopSequencer / AudioProcessor).
 *   It memory-maps a `.LOOP` file so the audio thread can read PCM data
 *   directly from mapped pages WITHOUT any heap allocation or file I/O.
 *
 * THE KEY INSIGHT: WHY MEMORY-MAP?
 * ──────────────────────────────────
 *   A sample bank may be hundreds of megabytes. If we loaded it into a
 *   std::vector<uint8_t>, we'd exhaust RAM quickly and delay startup.
 *
 *   With mmap:
 *     - open() returns almost instantly (no data is read at that point)
 *     - The OS loads only the 4KB pages the audio thread actually touches
 *     - Inactive samples cost exactly 0 bytes of physical RAM
 *     - Multiple Player instances share the same physical pages (copy-on-write)
 *
 * SAFE AUDIO-THREAD USAGE
 * ──────────────────────────
 *   getSamplePointer() returns a const pointer into the mapped region.
 *   This pointer is valid for the lifetime of the LoopFileReader object.
 *   The audio thread may call it without any lock, as long as:
 *     - close() is NOT called while the audio thread is running
 *     - The file on disk is NOT modified while mapped
 *
 *   Recommended shutdown sequence:
 *     1. Stop the audio device (LoopSequencer::releaseResources)
 *     2. Then call LoopFileReader::close()
 *
 * TYPICAL USAGE
 * ──────────────
 *   LoopFileReader reader;
 *
 *   // In prepareToPlay():
 *   auto result = reader.open(juce::File("/path/to/file.loop"));
 *   if (result != LoopFileReader::Error::None)
 *       // show error, stop
 *
 *   // In processBlock() (via LoopSequencer):
 *   const int16_t* pcm = reader.getSamplePointer(sampleID);
 *   uint32_t frames    = reader.getLUTEntry(sampleID).frame_count;
 *
 *   // In releaseResources() (before plugin destruction):
 *   reader.close();
 *
 * DEPENDENCIES
 *   - FormatSpec.h
 *   - JUCE modules: juce_core
 *
 * =============================================================================
 */

#pragma once

#include "FormatSpec.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace LoopFormat
{

class LoopFileReader
{
public:
    // ── Error Codes ──────────────────────────────────────────────────────────
    enum class Error
    {
        None = 0,
        FileNotFound,            ///< juce::File::existsAsFile() returned false
        MMapFailed,              ///< juce::MemoryMappedFile construction failed
        InvalidMagic,            ///< First 4 bytes are not {'L','O','O','P'}
        IncompatibleVersion,     ///< Major version mismatch
        HeaderCRCFailed,         ///< FileHeader CRC32 doesn't match stored value
        OffsetOutOfBounds,       ///< A chunk offset points past end of file
        LUTCRCFailed,            ///< (future) LUT integrity check failed
        SampleCRCFailed,         ///< PCM payload CRC32 mismatch
        InvalidSampleID,         ///< Caller passed an ID >= sample_count
        AlreadyOpen,             ///< open() called on an already-open reader
    };

    [[nodiscard]] static const char* describeError(Error e) noexcept;


    // ── Lifecycle ─────────────────────────────────────────────────────────────

    LoopFileReader()  = default;
    ~LoopFileReader() { close(); }

    // Not copyable (owns mmap handle + heap-allocated LUT cache)
    LoopFileReader(const LoopFileReader&)            = delete;
    LoopFileReader& operator=(const LoopFileReader&) = delete;

    // Movable
    LoopFileReader(LoopFileReader&&)            noexcept;
    LoopFileReader& operator=(LoopFileReader&&) noexcept;


    // ── Opening & closing ────────────────────────────────────────────────────

    /**
     * @brief Open and validate a `.LOOP` file.
     *
     * Steps performed:
     *   1. Verify the file exists
     *   2. Memory-map the entire file (READ_ONLY)
     *   3. Read and validate FileHeader (magic, version, CRC32)
     *   4. Validate all chunk offsets are within file bounds
     *   5. Build in-memory LUT cache (copy of SampleLUTEntry[])
     *   6. Verify each LUT entry's file_offset points to a valid
     *      SampleDataBlock with a matching sample_id
     *
     * Step 5 copies the LUT into a std::array so audio-thread lookups
     * are guaranteed to hit aligned memory (the mmap'd region is packed).
     *
     * Step 6 does NOT verify PCM CRC32 (that would read the whole file).
     * Use verifySampleCRC(id) to lazily verify individual samples.
     *
     * @param file  The .LOOP file to open.
     * @return      Error::None on success, or an error code.
     *
     * THREAD: Must be called from a non-audio thread (e.g. prepareToPlay
     *         if that runs on the message thread, or a background thread).
     */
    [[nodiscard]] Error open(const juce::File& file);

    /**
     * @brief Release the memory map and all cached data.
     *
     * MUST be called only after the audio thread has stopped reading
     * from this reader (i.e., after releaseResources() / device stop).
     *
     * Safe to call on an already-closed reader (no-op).
     */
    void close() noexcept;

    /**
     * @brief Returns true if the file is open and all header checks passed.
     *
     * Audio-thread safe (reads a std::atomic<bool>).
     */
    [[nodiscard]] bool isValid() const noexcept { return isValid_.load(std::memory_order_acquire); }


    // ── Header access ────────────────────────────────────────────────────────

    /**
     * @brief Returns a copy of the validated FileHeader.
     * Not audio-thread safe (returns by value; involves a memcpy).
     * Cache this in prepareToPlay() rather than calling in processBlock().
     */
    [[nodiscard]] FileHeader getFileHeader() const noexcept;

    /**
     * @brief Returns a copy of the SequenceHeader.
     * Same thread-safety note as getFileHeader().
     */
    [[nodiscard]] SequenceHeader getSequenceHeader() const noexcept;

    /**
     * @brief Total number of samples in the file.
     * Audio-thread safe (returns cached uint32_t).
     */
    [[nodiscard]] uint32_t getSampleCount() const noexcept { return sampleCount_; }

    /**
     * @brief Total number of sequence events in the file.
     * Audio-thread safe.
     */
    [[nodiscard]] uint32_t getEventCount() const noexcept { return eventCount_; }


    // ── LUT access (O(1), audio-thread safe) ─────────────────────────────────

    /**
     * @brief Returns a const reference to a LUT entry by sample ID.
     *
     * Audio-thread safe: reads from the pre-copied, aligned lutCache_ array.
     * No mmap access; no locks.
     *
     * @param sampleID  Must be < getSampleCount(). Unchecked in release builds.
     */
    [[nodiscard]] const SampleLUTEntry& getLUTEntry(uint16_t sampleID) const noexcept;


    // ── PCM data access (audio-thread safe) ──────────────────────────────────

    /**
     * @brief Returns a raw pointer into the memory-mapped PCM data.
     *
     * This is the hot path used by ActiveVoice for direct read access.
     *
     * The pointer points to the first PCM byte of the sample (i.e., it
     * already skips past the SampleDataBlock header).
     *
     * Layout: interleaved integer PCM, LE.
     *   For 16-bit stereo: [L0_lo, L0_hi, R0_lo, R0_hi, L1_lo, ...]
     *   For 24-bit mono:   [S0_b0, S0_b1, S0_b2, S1_b0, ...]
     *
     * @param sampleID  Must be < getSampleCount(). Unchecked in release.
     * @return          Const pointer into the mapped file region, or nullptr
     *                  if sampleID is out of range or reader is not valid.
     *
     * LIFETIME: Valid until close() is called. Never free() or delete[].
     */
    [[nodiscard]] const uint8_t* getSamplePointer(uint16_t sampleID) const noexcept;


    // ── Sequence event access (audio-thread safe) ─────────────────────────────

    /**
     * @brief Returns a const pointer to the start of the SequenceEvent array
     *        within the mapped file.
     *
     * The array has getEventCount() entries, sorted ascending by tick_start.
     * Use std::lower_bound with a tick comparator for efficient lookup.
     *
     * Example (in LoopSequencer):
     *   const SequenceEvent* begin = reader.getEventsBegin();
     *   const SequenceEvent* end   = reader.getEventsEnd();
     *   auto it = std::lower_bound(begin, end, targetTick, tickComparator);
     *
     * @return Pointer into the mmap'd region, or nullptr if not valid.
     */
    [[nodiscard]] const SequenceEvent* getEventsBegin() const noexcept;
    [[nodiscard]] const SequenceEvent* getEventsEnd()   const noexcept;


    // ── Lazy validation ───────────────────────────────────────────────────────

    /**
     * @brief Verify the CRC32 of a sample's PCM payload.
     *
     * This reads the full PCM data for the sample, which triggers OS page
     * faults. Call this from a background thread after open(), NOT from the
     * audio thread.
     *
     * @param sampleID  Sample to verify.
     * @return Error::None if CRC matches, Error::SampleCRCFailed otherwise.
     */
    [[nodiscard]] Error verifySampleCRC(uint16_t sampleID) const noexcept;

    /**
     * @brief Verify CRC32 for all samples. Background-thread only.
     *
     * Iterates verifySampleCRC() for each sample. Can take several seconds
     * for large files. Consider running in a juce::Thread.
     *
     * @param[out] failedID  If non-null, set to the first failing sample ID.
     * @return Error::None if all pass.
     */
    [[nodiscard]] Error verifyAllSampleCRCs(uint16_t* failedID = nullptr) const noexcept;


    // ── Meta access ───────────────────────────────────────────────────────────

    /**
     * @brief Look up a MetaChunk key=value entry.
     *
     * Returns an empty string if the key is not present or no MetaChunk exists.
     * Not audio-thread safe (involves string operations).
     *
     * @param key  The key to look up (e.g. "project", "s0.name").
     */
    [[nodiscard]] std::string getMetaValue(const std::string& key) const;


private:
    // ─────────────────────────────────────────────────────────────────────────
    //  Private helpers
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Validate that an offset + size falls within the mapped file.
     * Called during open() to catch truncated or corrupted files early.
     */
    [[nodiscard]] bool isOffsetValid(uint64_t offset, uint64_t size) const noexcept;

    /**
     * Read and verify the FileHeader CRC32.
     * Returns Error::None on success.
     */
    [[nodiscard]] Error validateHeaderCRC(const FileHeader& h) const noexcept;

    /**
     * Build the lutCache_ array from the packed mmap'd LUT region.
     * Copies each entry to ensure aligned access in the audio thread.
     */
    [[nodiscard]] Error buildLUTCache(const FileHeader& h);

    /**
     * Verify each LUT entry's file_offset points to a SampleDataBlock
     * with a matching sample_id tag.
     */
    [[nodiscard]] Error validateSampleOffsets(const FileHeader& h) const noexcept;

    /**
     * Parse the MetaChunk into metaMap_ (std::unordered_map<string,string>).
     * Called once during open(), not in the audio path.
     */
    void parseMetaChunk(const FileHeader& h);

    /**
     * Typed const-pointer into the mapped region at a given byte offset.
     * Returns nullptr if offset is out of bounds.
     *
     * AGENT NOTE: All mmap access must go through this helper so there is
     * one place to add bounds checking.
     */
    template<typename T>
    [[nodiscard]] const T* mappedPtr(uint64_t byteOffset) const noexcept
    {
        if (!mappedFile_ || !mappedFile_->getData()) return nullptr;
        const uint64_t fileSize = static_cast<uint64_t>(mappedFile_->getSize());
        if (byteOffset + sizeof(T) > fileSize) return nullptr;

        // Pointer arithmetic into the mapped region.
        // The void* cast is deliberate — we are intentionally reinterpreting
        // raw file bytes as a struct. #pragma pack(push,1) in FormatSpec.h
        // ensures no padding bytes exist in the struct layout.
        const auto* base = static_cast<const uint8_t*>(mappedFile_->getData());
        return reinterpret_cast<const T*>(base + byteOffset);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  State
    // ─────────────────────────────────────────────────────────────────────────

    /// The memory-mapped file handle. juce::MemoryMappedFile keeps the map
    /// alive for the object's lifetime. READ_ONLY prevents accidental writes.
    std::unique_ptr<juce::MemoryMappedFile> mappedFile_;

    /// Cached, aligned copy of the SampleLUT. Indexed by sampleID.
    /// Populated during open(); read-only after that.
    /// Using std::array (fixed-size, stack-compatible) rather than vector
    /// to avoid heap allocation and ensure O(1) access by ID.
    std::array<SampleLUTEntry, kMaxSamples> lutCache_ {};

    /// Parsed MetaChunk key→value pairs. Empty if no MetaChunk in file.
    /// juce::StringPairArray is used for JUCE-ecosystem compatibility.
    juce::StringPairArray metaMap_;

    /// Cached values from FileHeader (avoid re-reading mmap'd header).
    uint32_t sampleCount_  { 0 };
    uint32_t eventCount_   { 0 };
    uint64_t sequenceOffset_ { 0 };

    /// Atomic flag: set true only after ALL validation passes in open().
    /// Audio thread reads this; load with memory_order_acquire.
    std::atomic<bool> isValid_ { false };
};

} // namespace LoopFormat
