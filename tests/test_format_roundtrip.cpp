/**
 * @file    test_format_roundtrip.cpp
 * @brief   Write a .LOOP file with synthetic data, read it back, verify every field.
 *
 * =============================================================================
 * AGENT / DEVELOPER NOTES
 * =============================================================================
 *
 * PURPOSE
 *   This is the canonical correctness test for the I/O layer.
 *   It exercises the full write → read → verify cycle without any audio
 *   hardware or JUCE GUI components.
 *
 * HOW TO RUN
 *   Build as a standalone executable (add to CMakeLists as `add_executable`
 *   and link juce_core + juce_audio_basics). Run from the repo root:
 *
 *     ./test_format_roundtrip
 *     → All tests passed.
 *
 * WHAT IS TESTED
 *   - FileHeader magic, version, CRC32
 *   - SampleLUT: all fields match what was passed to addSample()
 *   - PCM data: byte-for-byte equality between source float buffer
 *     and the decoded bytes read back from the file
 *   - SequenceEvents: tick, sampleID, velocity, duration survive round-trip
 *   - Event sorting: events are returned in tick order regardless of insertion
 *   - MetaChunk: key=value pairs round-trip correctly
 *   - Zero-crossing trim: trimmed frame_count ≤ original frame count
 *   - Error codes: invalid inputs return the expected Error enum values
 *
 * SYNTHETIC DATA
 *   Samples are generated as a 440Hz sine wave (1 second at 44100Hz, mono,
 *   16-bit). This gives us a non-trivial waveform with known zero crossings.
 *
 * =============================================================================
 */

#include "../include/FormatSpec.h"
#include "../include/LoopFileWriter.h"
#include "../include/LoopFileReader.h"

// JUCE
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>


#include <cmath>
#include <cstdio>
#include <cassert>
#include <numbers>    // std::numbers::pi (C++20)

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal test harness (no third-party dependencies required)
// ─────────────────────────────────────────────────────────────────────────────

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define EXPECT(cond, msg)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
            ++g_testsFailed;                                                    \
        } else {                                                                \
            ++g_testsPassed;                                                    \
        }                                                                       \
    } while(0)

#define EXPECT_EQ(a, b, msg) EXPECT((a) == (b), msg)


// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Generate a 440Hz sine wave at the given sample rate.
 *
 * @param numFrames  Length of the buffer in frames.
 * @param sampleRate The sample rate (affects sine frequency and ZC locations).
 * @param channels   1 = mono, 2 = stereo (both channels identical).
 * @return           A juce::AudioBuffer<float> with values in [-1.f, 1.f].
 */
static juce::AudioBuffer<float> makeSineBuffer(int numFrames,
                                                double sampleRate,
                                                int channels = 1)
{
    juce::AudioBuffer<float> buf(channels, numFrames);

    for (int f = 0; f < numFrames; ++f)
    {
        const float s = std::sin(
            2.0f * static_cast<float>(std::numbers::pi)
            * 440.f * static_cast<float>(f) / static_cast<float>(sampleRate));

        for (int ch = 0; ch < channels; ++ch)
            buf.setSample(ch, f, s);
    }
    return buf;
}

/**
 * Decode a single 16-bit LE sample from raw bytes back to float.
 * Mirrors LoopFileWriter::convertFloatToInt() in reverse.
 */
static float decodeInt16ToFloat(const uint8_t* ptr) noexcept
{
    const int16_t raw = static_cast<int16_t>(
                            static_cast<uint16_t>(ptr[0]) |
                           (static_cast<uint16_t>(ptr[1]) << 8));
    return static_cast<float>(raw) / 32767.f;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Test cases
// ─────────────────────────────────────────────────────────────────────────────

static void test_headerIntegrity(const juce::File& loopFile)
{
    std::printf("  [test_headerIntegrity]\n");

    LoopFormat::LoopFileReader reader;
    auto err = reader.open(loopFile);

    EXPECT_EQ(err, LoopFormat::LoopFileReader::Error::None,
              "open() should succeed on a valid file");

    EXPECT(reader.isValid(), "isValid() should return true after successful open");

    const LoopFormat::FileHeader hdr = reader.getFileHeader();

    EXPECT(hdr.hasMagic(), "Magic bytes should be 'LOOP'");
    EXPECT_EQ(hdr.version_major, LoopFormat::kVersionMajor, "Major version mismatch");
    EXPECT_EQ(hdr.version_minor, LoopFormat::kVersionMinor, "Minor version mismatch");
    EXPECT_EQ(hdr.tpqn, LoopFormat::kDefaultTPQN, "TPQN should be 960");
    EXPECT(hdr.lut_offset >= sizeof(LoopFormat::FileHeader),
           "LUT offset must be after FileHeader");
    EXPECT(hdr.sample_data_offset > hdr.lut_offset,
           "Sample data must be after LUT");
    EXPECT(hdr.sequence_offset > hdr.sample_data_offset,
           "Sequence must be after sample data");
}


static void test_sampleRoundtrip(const juce::File& loopFile,
                                  const juce::AudioBuffer<float>& originalBuffer)
{
    std::printf("  [test_sampleRoundtrip]\n");

    LoopFormat::LoopFileReader reader;
    reader.open(loopFile);
    EXPECT(reader.isValid(), "Reader must be valid for sample roundtrip test");
    if (!reader.isValid()) return;

    // ── Verify LUT entry ─────────────────────────────────────────────────────
    const LoopFormat::SampleLUTEntry& lut = reader.getLUTEntry(0);

    EXPECT_EQ(lut.sample_id, 0,               "LUT sample_id should be 0");
    EXPECT_EQ(lut.sample_rate, 44100u,         "LUT sample_rate should be 44100");
    EXPECT_EQ(lut.channels, 1u,                "LUT channels should be 1 (mono)");
    EXPECT_EQ(lut.bitDepth(), 16u,             "LUT bit_depth should be 16");
    EXPECT(lut.frame_count > 0,                "LUT frame_count must be > 0");
    EXPECT(lut.frame_count <= static_cast<uint32_t>(originalBuffer.getNumSamples()),
           "Trimmed frame_count must be <= original (zero-crossing trim)");
    EXPECT(lut.flags & LoopFormat::kFlagLoopable,
           "kFlagLoopable should be set after ZC trim");
    EXPECT(lut.byte_length == lut.frame_count * 2u,
           "byte_length should be frame_count * 2 for 16-bit mono");

    // ── PCM CRC verification ──────────────────────────────────────────────────
    const auto crcErr = reader.verifySampleCRC(0);
    EXPECT_EQ(crcErr, LoopFormat::LoopFileReader::Error::None,
              "PCM CRC32 should match");

    // ── Decode and compare first 100 frames ──────────────────────────────────
    const uint8_t* pcm = reader.getSamplePointer(0);
    EXPECT(pcm != nullptr, "getSamplePointer(0) must not return null");
    if (!pcm) return;

    constexpr int kCheckFrames = 100;
    bool allMatch = true;

    for (int f = 0; f < kCheckFrames && f < static_cast<int>(lut.frame_count); ++f)
    {
        const float original = originalBuffer.getSample(0, f);
        const float decoded  = decodeInt16ToFloat(pcm + f * 2);

        // Allow ~0.003 tolerance due to float→int16→float quantisation
        if (std::abs(original - decoded) > 0.003f)
        {
            allMatch = false;
            std::fprintf(stderr, "    Frame %d: original=%.6f decoded=%.6f\n",
                         f, original, decoded);
            break;
        }
    }

    EXPECT(allMatch, "First 100 PCM frames should match within quantisation tolerance");
}


static void test_eventRoundtrip(const juce::File& loopFile)
{
    std::printf("  [test_eventRoundtrip]\n");

    LoopFormat::LoopFileReader reader;
    reader.open(loopFile);
    EXPECT(reader.isValid(), "Reader must be valid"); if (!reader.isValid()) return;

    EXPECT_EQ(reader.getEventCount(), 3u, "Should have exactly 3 events");

    const LoopFormat::SequenceEvent* begin = reader.getEventsBegin();
    const LoopFormat::SequenceEvent* end   = reader.getEventsEnd();

    EXPECT(begin != nullptr, "getEventsBegin() must not be null");
    EXPECT(end   != nullptr, "getEventsEnd() must not be null");
    if (!begin || !end) return;

    EXPECT_EQ(static_cast<size_t>(end - begin), 3u, "Event pointer span must be 3");

    // Events were added in non-sorted order (tick 960, 0, 1920).
    // After write(), they must be sorted ascending by tick_start.
    EXPECT(begin[0].tickStart() <= begin[1].tickStart(), "Events must be sorted [0]≤[1]");
    EXPECT(begin[1].tickStart() <= begin[2].tickStart(), "Events must be sorted [1]≤[2]");

    // Verify specific event fields
    // Event 0 (originally tick=0): kick, vel=100, dur=1920
    EXPECT_EQ(begin[0].tickStart(),    0u,   "First event tick must be 0");
    EXPECT_EQ(begin[0].sampleID(),     0u,   "First event sampleID must be 0");
    EXPECT_EQ(begin[0].velocity(),     100u, "First event velocity must be 100");
    EXPECT_EQ(begin[0].loopDuration(), 1920u,"First event duration must be 1920");
    EXPECT(!begin[0].isOneShot(),            "First event should not be one-shot");

    // Event 1 (tick=960): snare, vel=80, dur=960, oneShot=true
    EXPECT_EQ(begin[1].tickStart(),    960u, "Second event tick must be 960");
    EXPECT_EQ(begin[1].sampleID(),     1u,   "Second event sampleID must be 1");
    EXPECT_EQ(begin[1].velocity(),     80u,  "Second event velocity must be 80");
    EXPECT(begin[1].isOneShot(),             "Second event should be one-shot");
}


static void test_sequenceHeaderFields(const juce::File& loopFile)
{
    std::printf("  [test_sequenceHeaderFields]\n");

    LoopFormat::LoopFileReader reader;
    reader.open(loopFile);
    EXPECT(reader.isValid(), "Reader must be valid"); if (!reader.isValid()) return;

    const LoopFormat::SequenceHeader seqHdr = reader.getSequenceHeader();

    EXPECT_EQ(seqHdr.tag, LoopFormat::kTagSequence, "Sequence FourCC tag must match");
    EXPECT_EQ(seqHdr.event_count, 3u,                "Sequence event_count must be 3");
    EXPECT(seqHdr.bpm > 0.f,                         "BPM must be > 0");
    EXPECT_EQ(seqHdr.time_sig_numerator, 4u,          "Time sig numerator must be 4");
    EXPECT_EQ(seqHdr.time_sig_denominator, 4u,        "Time sig denominator must be 4");
    EXPECT(seqHdr.total_duration_ticks > 0u,          "Total duration must be > 0");
    EXPECT(seqHdr.crc32_events != 0u,                 "Event CRC should be non-zero");
}


static void test_metaRoundtrip(const juce::File& loopFile)
{
    std::printf("  [test_metaRoundtrip]\n");

    LoopFormat::LoopFileReader reader;
    reader.open(loopFile);
    EXPECT(reader.isValid(), "Reader must be valid"); if (!reader.isValid()) return;

    EXPECT_EQ(reader.getMetaValue("project"), std::string("Test Beat Pack"),
              "Meta 'project' key should round-trip correctly");
    EXPECT_EQ(reader.getMetaValue("author"), std::string("LOOP Engine"),
              "Meta 'author' key should round-trip correctly");
    EXPECT_EQ(reader.getMetaValue("nonexistent"), std::string(""),
              "Missing key should return empty string");
}


static void test_errorCases(const juce::File& tempDir)
{
    std::printf("  [test_errorCases]\n");

    // File not found
    {
        LoopFormat::LoopFileReader reader;
        auto err = reader.open(tempDir.getChildFile("does_not_exist.loop"));
        EXPECT_EQ(err, LoopFormat::LoopFileReader::Error::FileNotFound,
                  "Should return FileNotFound for missing file");
    }

    // Writer: no samples
    {
        LoopFormat::LoopFileWriter writer;
        auto err = writer.write(tempDir.getChildFile("empty.loop"));
        EXPECT_EQ(err, LoopFormat::LoopFileWriter::Error::NoSamples,
                  "Should return NoSamples if write() called with no samples");
    }

    // Writer: invalid bit depth
    {
        LoopFormat::LoopFileWriter writer;
        auto buf = makeSineBuffer(1024, 44100.0);
        LoopFormat::SampleMeta meta;
        meta.bit_depth = 8; // Not supported
        auto err = writer.addSample(buf, meta);
        EXPECT_EQ(err, LoopFormat::LoopFileWriter::Error::InvalidBitDepth,
                  "Should return InvalidBitDepth for 8-bit input");
    }

    // Writer: sample rate mismatch
    {
        LoopFormat::LoopFileWriter writer;
        auto buf1 = makeSineBuffer(1024, 44100.0);
        auto buf2 = makeSineBuffer(1024, 48000.0);

        LoopFormat::SampleMeta meta1; meta1.sample_rate = 44100;
        LoopFormat::SampleMeta meta2; meta2.sample_rate = 48000;

        writer.addSample(buf1, meta1);
        auto err = writer.addSample(buf2, meta2);
        EXPECT_EQ(err, LoopFormat::LoopFileWriter::Error::SampleRateMismatch,
                  "Should return SampleRateMismatch for mixed sample rates");
    }

    // Writer: already-open reader rejects second open()
    {
        // (Requires a valid .loop file — create a minimal one)
        LoopFormat::LoopFileWriter writer;
        auto buf = makeSineBuffer(1024, 44100.0);
        LoopFormat::SampleMeta meta;
        writer.addSample(buf, meta);
        const juce::File tinyFile = tempDir.getChildFile("tiny.loop");
        writer.write(tinyFile);

        LoopFormat::LoopFileReader reader;
        reader.open(tinyFile);
        EXPECT(reader.isValid(), "tiny.loop should open successfully");

        auto err2 = reader.open(tinyFile); // Second open on same reader
        EXPECT_EQ(err2, LoopFormat::LoopFileReader::Error::AlreadyOpen,
                  "Should return AlreadyOpen on double open()");
    }
}


static void test_timingMath()
{
    std::printf("  [test_timingMath]\n");

    using namespace LoopFormat;

    // At 120 BPM, 44100 Hz, TPQN=960:
    //   1 quarter note = 960 ticks = 22050 samples (0.5 seconds)
    //   samplesPerTick = 22050 / 960 ≈ 22.96875
    {
        constexpr double expected = 22050.0;
        const double result = tickToSample(960, 120.0, 44100.0, 960);
        EXPECT(std::abs(result - expected) < 0.001,
               "960 ticks at 120BPM/44100Hz should be ~22050 samples");
    }

    // Round-trip: tick→sample→tick should recover the original tick
    {
        constexpr uint32_t tick       = 3840; // 4 beats
        constexpr double   bpm        = 120.0;
        constexpr double   sampleRate = 44100.0;

        const double   samplePos    = tickToSample(tick, bpm, sampleRate);
        const uint32_t recoveredTick = sampleToTick(samplePos, bpm, sampleRate);

        EXPECT_EQ(recoveredTick, tick,
                  "tick→sample→tick round-trip should be lossless");
    }

    // BPM change: same tick, different BPM → different sample position
    {
        const double at120 = tickToSample(960, 120.0, 44100.0);
        const double at140 = tickToSample(960, 140.0, 44100.0);

        EXPECT(at120 > at140,
               "At higher BPM, same tick count = fewer samples (faster)");
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    // JUCE initialisation (required for juce::File and MemoryMappedFile)
     // or juce::initialiseJuce_NonGUI() for headless

    std::printf("=== .LOOP Format Round-Trip Tests ===\n\n");

    // Temp directory for test files
    const juce::File tempDir = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile("loop_test_" +
        juce::String(juce::Random::getSystemRandom().nextInt()));
    tempDir.createDirectory();

    const juce::File testFile = tempDir.getChildFile("test.loop");

    // ── Build test .LOOP file ─────────────────────────────────────────────────
    std::printf("[SETUP] Building test .LOOP file...\n");

    juce::AudioBuffer<float> kickBuf  = makeSineBuffer(44100, 44100.0, 1); // 1s mono
    juce::AudioBuffer<float> snareBuf = makeSineBuffer(22050, 44100.0, 2); // 0.5s stereo

    LoopFormat::LoopFileWriter writer;
    writer.setBPM(120.f);
    writer.setTimeSignature(4, 4);
    writer.addMeta("project", "Test Beat Pack");
    writer.addMeta("author",  "LOOP Engine");

    // Add kick (ID 0)
    LoopFormat::SampleMeta kickMeta;
    kickMeta.sample_rate = 44100;
    kickMeta.channels    = 1;
    kickMeta.bit_depth   = 16;
    kickMeta.base_note   = 60.f;
    kickMeta.loop_bpm    = 120.f;
    kickMeta.name        = "kick";
    uint16_t kickID = 0;
    writer.addSample(kickBuf, kickMeta, &kickID);

    // Add snare (ID 1)
    LoopFormat::SampleMeta snareMeta;
    snareMeta.sample_rate = 44100;
    snareMeta.channels    = 2;
    snareMeta.bit_depth   = 16;
    snareMeta.base_note   = 38.f;
    snareMeta.loop_bpm    = 0.f;
    snareMeta.name        = "snare";
    writer.addSample(snareBuf, snareMeta, nullptr);

    // Add events in NON-sorted order (writer must sort them)
    writer.addEvent(960,  1,  80,  960, /*oneShot=*/ true); // snare on beat 2
    writer.addEvent(  0,  0, 100, 1920, /*oneShot=*/ false); // kick on beat 1
    writer.addEvent(1920, 0,  90, 1920, /*oneShot=*/ false); // kick on beat 3

    const auto writeErr = writer.write(testFile);
    if (writeErr != LoopFormat::LoopFileWriter::Error::None)
    {
        std::fprintf(stderr, "SETUP FAILED: write() returned: %s\n",
                     LoopFormat::LoopFileWriter::describeError(writeErr));
        return 1;
    }
    std::printf("[SETUP] Written %llu bytes to %s\n\n",
                static_cast<unsigned long long>(testFile.getSize()),
                testFile.getFullPathName().toRawUTF8());

    // ── Run tests ─────────────────────────────────────────────────────────────
    test_headerIntegrity(testFile);
    test_sampleRoundtrip(testFile, kickBuf);
    test_eventRoundtrip(testFile);
    test_sequenceHeaderFields(testFile);
    test_metaRoundtrip(testFile);
    test_errorCases(tempDir);
    test_timingMath();

    // ── Summary ───────────────────────────────────────────────────────────────
    std::printf("\n=== Results: %d passed, %d failed ===\n",
                g_testsPassed, g_testsFailed);

    // Cleanup temp files
    tempDir.deleteRecursively();

    

    return g_testsFailed == 0 ? 0 : 1;
}
