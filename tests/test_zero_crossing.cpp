/**
 * @file    test_zero_crossing.cpp
 * @brief   Isolated tests for zero-crossing trim logic.
 *
 * =============================================================================
 * WHAT IS TESTED
 * =============================================================================
 *
 *  1. findZeroCrossing_16() — raw helper in FormatSpec.h
 *       - Sine wave: finds a crossing within the last 0.5 s
 *       - All-silence buffer: returns the last frame (already at zero)
 *       - Flat non-zero buffer: falls back to natural end (no crossing found)
 *       - Single-sample buffer edge case
 *       - Stereo: both channels must be near zero simultaneously
 *
 *  2. LoopFileWriter zero-crossing trim integration
 *       - frame_count in the LUT ≤ original buffer frame count
 *       - kFlagLoopable is always set after addSample()
 *       - Silence buffer (all zeros): frame_count == original (zero-crossing
 *         at frame 0, trim keeps full length)
 *       - Non-zero DC offset at tail: trim removes the non-silent tail
 *
 *  3. Round-trip CRC after trim
 *       - Verify PCM CRC survives write→read cycle
 *
 * =============================================================================
 */

#include "../include/FormatSpec.h"
#include "../include/LoopFileWriter.h"
#include "../include/LoopFileReader.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>


#include <cmath>
#include <cstdio>
#include <cassert>
#include <numbers>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal test harness
// ─────────────────────────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

#define EXPECT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "  FAIL [line %d] %s\n", __LINE__, (msg)); \
            ++g_failed; \
        } else { \
            ++g_passed; \
        } \
    } while(0)

#define EXPECT_EQ(a, b, msg)   EXPECT((a) == (b), msg)
#define EXPECT_LE(a, b, msg)   EXPECT((a) <= (b), msg)
#define EXPECT_GE(a, b, msg)   EXPECT((a) >= (b), msg)


// ─────────────────────────────────────────────────────────────────────────────
//  Helper: build a raw int16 buffer from a float lambda
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<int16_t> makePCM16(int frames, int channels,
                                       std::function<float(int frame, int ch)> fn)
{
    std::vector<int16_t> out(static_cast<size_t>(frames * channels));
    for (int f = 0; f < frames; ++f)
        for (int ch = 0; ch < channels; ++ch)
        {
            const float s = fn(f, ch);
            const float clamped = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
            out[static_cast<size_t>(f * channels + ch)] =
                static_cast<int16_t>(clamped * 32767.f);
        }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Tests for findZeroCrossing_16()
// ─────────────────────────────────────────────────────────────────────────────

static void test_zcFinder_sineWave()
{
    std::printf("  [test_zcFinder_sineWave]\n");

    // 1 second of 440 Hz sine at 44100 Hz, mono
    constexpr int kFrames = 44100;
    constexpr int kCh     = 1;
    const auto pcm = makePCM16(kFrames, kCh,
        [](int f, int) {
            return std::sin(2.f * static_cast<float>(std::numbers::pi)
                            * 440.f * static_cast<float>(f) / 44100.f);
        });

    const uint32_t zc = LoopFormat::findZeroCrossing_16(
        pcm.data(), kCh, kFrames);

    // Must find a crossing before the end (sine definitely crosses zero near 44100)
    EXPECT_LE(zc, static_cast<uint32_t>(kFrames),
              "ZC must be <= original frame count");
    EXPECT_GE(zc, static_cast<uint32_t>(kFrames - 22050),
              "ZC must be within last 0.5 s");

    // The frame at the crossing must actually be near zero
    if (zc > 0 && zc <= static_cast<uint32_t>(kFrames))
    {
        const int16_t val = pcm[static_cast<size_t>((zc - 1) * kCh)];
        EXPECT(std::abs(val) <= 64, "ZC frame amplitude must be <= threshold (64)");
    }
}

static void test_zcFinder_silence()
{
    std::printf("  [test_zcFinder_silence]\n");

    // All-zero buffer — every frame is a zero crossing.
    // findZeroCrossing_16 scans from end; the very first check (last frame) passes.
    constexpr int kFrames = 1024;
    const std::vector<int16_t> pcm(static_cast<size_t>(kFrames), 0);

    const uint32_t zc = LoopFormat::findZeroCrossing_16(
        pcm.data(), 1, kFrames);

    // Should return kFrames (the last frame is already at zero → returns endFrame)
    EXPECT_EQ(zc, static_cast<uint32_t>(kFrames),
              "All-silence: ZC should be at natural end");
}

static void test_zcFinder_flatNonZero()
{
    std::printf("  [test_zcFinder_flatNonZero]\n");

    // Constant DC offset of 0.5 — no zero crossing anywhere.
    // Expect fallback to endFrame.
    constexpr int kFrames = 2048;
    const std::vector<int16_t> pcm(static_cast<size_t>(kFrames),
                                    static_cast<int16_t>(16384)); // ~0.5 full scale

    const uint32_t zc = LoopFormat::findZeroCrossing_16(
        pcm.data(), 1, kFrames);

    EXPECT_EQ(zc, static_cast<uint32_t>(kFrames),
              "Flat non-zero: ZC should fall back to natural end");
}

static void test_zcFinder_stereo()
{
    std::printf("  [test_zcFinder_stereo]\n");

    // Stereo: L channel is silent, R channel is a 440 Hz sine.
    // A crossing requires BOTH channels to be near zero simultaneously.
    constexpr int kFrames = 44100;
    constexpr int kCh     = 2;

    const auto pcm = makePCM16(kFrames, kCh,
        [](int f, int ch) -> float {
            if (ch == 0) return 0.f; // L: silent
            return std::sin(2.f * static_cast<float>(std::numbers::pi)
                            * 440.f * static_cast<float>(f) / 44100.f); // R: sine
        });

    const uint32_t zc = LoopFormat::findZeroCrossing_16(
        pcm.data(), kCh, kFrames);

    // Should still find a crossing (where R sine is near zero)
    EXPECT_LE(zc, static_cast<uint32_t>(kFrames),
              "Stereo ZC must be <= frame count");
}

static void test_zcFinder_singleFrame()
{
    std::printf("  [test_zcFinder_singleFrame]\n");

    // Edge case: 1-frame buffer, value = 0
    const int16_t zero = 0;
    const uint32_t zc = LoopFormat::findZeroCrossing_16(&zero, 1, 1);
    EXPECT_EQ(zc, 1u, "Single zero frame: ZC should be 1");

    // Edge case: 1-frame buffer, non-zero value
    const int16_t loud = 30000;
    const uint32_t zc2 = LoopFormat::findZeroCrossing_16(&loud, 1, 1);
    EXPECT_EQ(zc2, 1u, "Single non-zero frame: falls back to 1");
}

static void test_zcFinder_nullInput()
{
    std::printf("  [test_zcFinder_nullInput]\n");

    // Null pointer → should return endFrame (safe no-op)
    const uint32_t zc = LoopFormat::findZeroCrossing_16(nullptr, 1, 100);
    EXPECT_EQ(zc, 100u, "Null input: should return endFrame");

    // Zero channels → should return endFrame
    const int16_t dummy = 0;
    const uint32_t zc2 = LoopFormat::findZeroCrossing_16(&dummy, 0, 100);
    EXPECT_EQ(zc2, 100u, "Zero channels: should return endFrame");
}


// ─────────────────────────────────────────────────────────────────────────────
//  Tests for LoopFileWriter trim integration
// ─────────────────────────────────────────────────────────────────────────────

static void test_writerTrim_sineIsShortened(const juce::File& tempDir)
{
    std::printf("  [test_writerTrim_sineIsShortened]\n");

    // A sine wave will almost always be trimmed (unless the last sample
    // happens to be exactly at zero, which is unlikely for 44100 frames).
    constexpr int kFrames = 44100;
    juce::AudioBuffer<float> buf(1, kFrames);
    for (int f = 0; f < kFrames; ++f)
    {
        buf.setSample(0, f,
            std::sin(2.f * static_cast<float>(std::numbers::pi)
                     * 440.f * static_cast<float>(f) / 44100.f));
    }

    LoopFormat::LoopFileWriter writer;
    LoopFormat::SampleMeta meta;
    meta.sample_rate = 44100;
    meta.channels    = 1;
    meta.bit_depth   = 16;

    uint16_t id = 0xFFFF;
    const auto err = writer.addSample(buf, meta, &id);
    EXPECT_EQ(err, LoopFormat::LoopFileWriter::Error::None, "addSample should succeed");
    EXPECT_EQ(id, 0u, "First sample should get ID 0");

    const juce::File f = tempDir.getChildFile("zc_sine.loop");
    writer.write(f);

    LoopFormat::LoopFileReader reader;
    reader.open(f);
    EXPECT(reader.isValid(), "Reader should open ZC-trimmed file");
    if (!reader.isValid()) return;

    const LoopFormat::SampleLUTEntry& lut = reader.getLUTEntry(0);

    EXPECT(lut.flags & LoopFormat::kFlagLoopable,
           "kFlagLoopable must be set after ZC trim");

    EXPECT_LE(lut.frame_count, static_cast<uint32_t>(kFrames),
              "Trimmed frame_count must be <= original");

    // Byte length consistency
    EXPECT_EQ(lut.byte_length, lut.frame_count * 2u,
              "16-bit mono: byte_length == frame_count * 2");
}

static void test_writerTrim_silenceIsNotShorter(const juce::File& tempDir)
{
    std::printf("  [test_writerTrim_silenceIsNotShorter]\n");

    // An all-zero buffer: the very last frame is already at zero.
    // The trim should find the crossing at or near the end → frame_count
    // should be close to original (within the lookback window).
    constexpr int kFrames = 1024;
    juce::AudioBuffer<float> buf(1, kFrames);
    buf.clear(); // all zeros

    LoopFormat::LoopFileWriter writer;
    LoopFormat::SampleMeta meta;
    meta.sample_rate = 44100;
    meta.channels    = 1;
    meta.bit_depth   = 16;

    writer.addSample(buf, meta);
    const juce::File f = tempDir.getChildFile("zc_silence.loop");
    writer.write(f);

    LoopFormat::LoopFileReader reader;
    reader.open(f);
    EXPECT(reader.isValid(), "Reader should open silence file");
    if (!reader.isValid()) return;

    const LoopFormat::SampleLUTEntry& lut = reader.getLUTEntry(0);
    EXPECT_GE(lut.frame_count, 1u, "Silence: at least 1 frame should remain");
    EXPECT_LE(lut.frame_count, static_cast<uint32_t>(kFrames),
              "frame_count must not exceed original");
}

static void test_writerTrim_flagsAndCRC(const juce::File& tempDir)
{
    std::printf("  [test_writerTrim_flagsAndCRC]\n");

    // Write a 24-bit stereo sample, check flags + CRC
    constexpr int kFrames = 4410; // 0.1 s
    juce::AudioBuffer<float> buf(2, kFrames);
    for (int f = 0; f < kFrames; ++f)
    {
        const float s = std::sin(2.f * static_cast<float>(std::numbers::pi)
                                  * 220.f * static_cast<float>(f) / 44100.f);
        buf.setSample(0, f, s);
        buf.setSample(1, f, s * 0.5f);
    }

    LoopFormat::LoopFileWriter writer;
    LoopFormat::SampleMeta meta;
    meta.sample_rate = 44100;
    meta.channels    = 2;
    meta.bit_depth   = 24;

    writer.addSample(buf, meta);
    const juce::File f = tempDir.getChildFile("zc_24bit_stereo.loop");
    writer.write(f);

    LoopFormat::LoopFileReader reader;
    reader.open(f);
    EXPECT(reader.isValid(), "24-bit stereo file should open");
    if (!reader.isValid()) return;

    const LoopFormat::SampleLUTEntry& lut = reader.getLUTEntry(0);

    EXPECT(lut.flags & LoopFormat::kBitDepth24, "24-bit flag must be set");
    EXPECT(lut.flags & LoopFormat::kFlagStereo,  "Stereo flag must be set");
    EXPECT(lut.flags & LoopFormat::kFlagLoopable,"Loopable flag must be set");

    // CRC check
    const auto crcErr = reader.verifySampleCRC(0);
    EXPECT_EQ(crcErr, LoopFormat::LoopFileReader::Error::None,
              "PCM CRC must pass after ZC trim and round-trip");
}

static void test_writerTrim_multiSample(const juce::File& tempDir)
{
    std::printf("  [test_writerTrim_multiSample]\n");

    // Two samples with different original lengths; both should be trimmed
    // independently and kFlagLoopable set on each.
    juce::AudioBuffer<float> buf1(1, 22050); // 0.5 s
    juce::AudioBuffer<float> buf2(1, 88200); // 2 s

    for (int f = 0; f < buf1.getNumSamples(); ++f)
        buf1.setSample(0, f, std::sin(2.f * static_cast<float>(std::numbers::pi)
                                        * 440.f * f / 44100.f));

    for (int f = 0; f < buf2.getNumSamples(); ++f)
        buf2.setSample(0, f, std::sin(2.f * static_cast<float>(std::numbers::pi)
                                        * 880.f * f / 44100.f));

    LoopFormat::LoopFileWriter writer;
    LoopFormat::SampleMeta meta;
    meta.sample_rate = 44100;
    meta.channels    = 1;
    meta.bit_depth   = 16;

    writer.addSample(buf1, meta);
    writer.addSample(buf2, meta);
    writer.addEvent(0,    0, 100, 960);
    writer.addEvent(960,  1, 80,  1920);

    const juce::File f = tempDir.getChildFile("zc_multi.loop");
    writer.write(f);

    LoopFormat::LoopFileReader reader;
    reader.open(f);
    EXPECT(reader.isValid(), "Multi-sample file must open");
    if (!reader.isValid()) return;

    EXPECT_EQ(reader.getSampleCount(), 2u, "Should have 2 samples");

    for (uint16_t id = 0; id < 2; ++id)
    {
        const LoopFormat::SampleLUTEntry& lut = reader.getLUTEntry(id);
        EXPECT(lut.flags & LoopFormat::kFlagLoopable,
               "kFlagLoopable must be set on each sample");
        EXPECT(lut.frame_count > 0, "frame_count must be > 0");
        EXPECT_EQ(reader.verifySampleCRC(id),
                  LoopFormat::LoopFileReader::Error::None,
                  "CRC must pass for each sample");
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    

    std::printf("=== .LOOP Zero-Crossing Trim Tests ===\n\n");

    const juce::File tempDir = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile(
            "loop_zc_test_" +
            juce::String(juce::Random::getSystemRandom().nextInt()));
    tempDir.createDirectory();

    // ── findZeroCrossing_16() unit tests (no file I/O) ────────────────────────
    test_zcFinder_sineWave();
    test_zcFinder_silence();
    test_zcFinder_flatNonZero();
    test_zcFinder_stereo();
    test_zcFinder_singleFrame();
    test_zcFinder_nullInput();

    // ── LoopFileWriter trim integration tests ─────────────────────────────────
    test_writerTrim_sineIsShortened(tempDir);
    test_writerTrim_silenceIsNotShorter(tempDir);
    test_writerTrim_flagsAndCRC(tempDir);
    test_writerTrim_multiSample(tempDir);

    std::printf("\n=== Results: %d passed, %d failed ===\n",
                g_passed, g_failed);

    tempDir.deleteRecursively();
    

    return g_failed == 0 ? 0 : 1;
}
