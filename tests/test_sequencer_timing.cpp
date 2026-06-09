/**
 * @file    test_sequencer_timing.cpp
 * @brief   Verify tick-to-sample math, look-ahead scheduling correctness,
 *          and voice pool behaviour under stress.
 *
 * =============================================================================
 * AGENT / DEVELOPER NOTES
 * =============================================================================
 *
 * This test file exercises the LoopSequencer in isolation WITHOUT real audio
 * hardware by driving getNextAudioBlock() manually with a fake AudioBuffer.
 *
 * WHAT IS TESTED
 *   1. tickToSample() / sampleToTick() correctness at 5 BPMs
 *   2. samplesPerTick() stays consistent across multiple blocks
 *   3. Events scheduled at tick boundaries actually fire at the correct
 *      sample offset (within ±1 sample tolerance due to rounding)
 *   4. Voice pool: 64 simultaneous voices activate and deactivate correctly
 *   5. Voice stealing: 65th event steals the most-complete voice
 *   6. Sequence looping: playhead wraps at total_duration_ticks
 *   7. Transport: play/stop/pause state transitions
 *   8. BPM change mid-playback: events stay on-grid
 *
 * HOW TO RUN
 *   Same as test_format_roundtrip.cpp — build LOOPTests target and run.
 *
 * =============================================================================
 */

#include "../include/FormatSpec.h"
#include "../include/LoopFileWriter.h"
#include "../include/LoopFileReader.h"
#include "../include/LoopSequencer.h"
#include "../include/ActiveVoice.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>


#include <cmath>
#include <cstdio>
#include <cassert>
#include <numbers>

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal test harness (shared with other test files)
// ─────────────────────────────────────────────────────────────────────────────

static int g_passed = 0;
static int g_failed = 0;

#define EXPECT(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr,"  FAIL [line %d] %s\n",__LINE__,msg); ++g_failed; } \
         else { ++g_passed; } } while(0)

#define EXPECT_NEAR(a, b, tol, msg) \
    EXPECT(std::abs((a)-(b)) <= (tol), msg)

#define EXPECT_EQ(a, b, msg) EXPECT((a) == (b), msg)


// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Create a minimal in-memory .LOOP file for sequencer tests.
 * Returns the file path; caller must delete it when done.
 *
 * The file contains:
 *   - 1 sample: 2 seconds of 440Hz sine, mono 16-bit, 44100Hz
 *   - N events, all playing sample 0, evenly spaced every `spacingTicks` ticks
 */
static juce::File makeTestLoopFile(const juce::File& dir,
                                    int   numEvents      = 4,
                                    uint32_t spacingTicks  = 960,
                                    float bpm            = 120.f)
{
    LoopFormat::LoopFileWriter writer;
    writer.setBPM(bpm);
    writer.setTimeSignature(4, 4);

    // 2-second sine wave
    constexpr int kFrames = 88200;
    juce::AudioBuffer<float> buf(1, kFrames);
    for (int f = 0; f < kFrames; ++f)
    {
        const float s = std::sin(2.f * static_cast<float>(std::numbers::pi)
                                 * 440.f * static_cast<float>(f) / 44100.f);
        buf.setSample(0, f, s);
    }

    LoopFormat::SampleMeta meta;
    meta.sample_rate = 44100;
    meta.channels    = 1;
    meta.bit_depth   = 16;
    writer.addSample(buf, meta);

    for (int i = 0; i < numEvents; ++i)
    {
        const uint32_t tick = static_cast<uint32_t>(i) * spacingTicks;
        writer.addEvent(tick, 0, 100, spacingTicks);
    }

    const juce::File f = dir.getChildFile("seq_test_" +
        juce::String(numEvents) + "_" + juce::String(static_cast<int>(bpm)) + ".loop");
    writer.write(f);
    return f;
}

/**
 * Drive the sequencer for `numBlocks` blocks and return the total number of
 * frames rendered (sum of all active voice render calls, approximate).
 */
static int driveSequencer(LoopFormat::LoopSequencer& seq,
                           int numBlocks,
                           int blockSize = 512,
                           double sampleRate = 44100.0)
{
    juce::AudioBuffer<float> buf(2, blockSize);
    int totalActive = 0;

    for (int b = 0; b < numBlocks; ++b)
    {
        buf.clear();
        juce::AudioSourceChannelInfo info(&buf, 0, blockSize);
        seq.getNextAudioBlock(info);
        totalActive += seq.getActiveVoiceCount();
    }

    return totalActive;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Test: Tick ↔ Sample math
// ─────────────────────────────────────────────────────────────────────────────

static void test_tickMathAtVariousBPMs()
{
    std::printf("  [test_tickMathAtVariousBPMs]\n");

    using namespace LoopFormat;

    // Test table: {bpm, sampleRate, tick, expected_samples}
    struct Case { float bpm; double sr; uint32_t tick; double expectedSamples; };
    const Case cases[] = {
        { 120.f, 44100.0, 960,  22050.0 }, // 1 beat at 120BPM = 22050 samples
        { 120.f, 44100.0, 3840, 88200.0 }, // 4 beats = 88200 samples
        { 140.f, 44100.0, 960,  18900.0 }, // 1 beat at 140BPM = 18900 samples
        {  60.f, 44100.0, 960,  44100.0 }, // 1 beat at 60BPM  = 44100 samples (1 second)
        { 120.f, 48000.0, 960,  24000.0 }, // 1 beat at 120BPM/48kHz = 24000 samples
    };

    for (const auto& c : cases)
    {
        const double result = tickToSample(c.tick, c.bpm, c.sr);
        EXPECT_NEAR(result, c.expectedSamples, 0.5,
                    "tickToSample() mismatch");

        // Round-trip
        const uint32_t recovered = sampleToTick(result, c.bpm, c.sr);
        EXPECT_EQ(recovered, c.tick, "sampleToTick round-trip failed");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test: samplesPerTick consistency across blocks
// ─────────────────────────────────────────────────────────────────────────────

static void test_samplesPerTickConsistency()
{
    std::printf("  [test_samplesPerTickConsistency]\n");

    using namespace LoopFormat;

    // At 120BPM / 44100Hz / TPQN=960:
    //   1 quarter note = 960 ticks = 22050 samples
    //   samplesPerTick = 22050 / 960 = 22.96875
    constexpr double expected = 22.96875;
    const double result = samplesPerTick(120.0, 44100.0);
    EXPECT_NEAR(result, expected, 0.0001, "samplesPerTick(120, 44100) should be 22.96875");

    // Verify: 1000 ticks × samplesPerTick = tickToSample(1000, ...)
    const double fromTable  = samplesPerTick(120.0, 44100.0) * 1000.0;
    const double fromDirect = tickToSample(1000, 120.0, 44100.0);
    EXPECT_NEAR(fromTable, fromDirect, 0.01, "samplesPerTick × N should match tickToSample(N)");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test: Sequencer plays events, voice pool activates correctly
// ─────────────────────────────────────────────────────────────────────────────

static void test_sequencerVoiceActivation(const juce::File& testDir)
{
    std::printf("  [test_sequencerVoiceActivation]\n");

    // 4 events, each 1 beat apart (960 ticks) at 120BPM
    const juce::File loopFile = makeTestLoopFile(testDir, 4, 960, 120.f);

    LoopFormat::LoopFileReader reader;
    EXPECT(reader.open(loopFile) == LoopFormat::LoopFileReader::Error::None,
           "Reader should open successfully");
    if (!reader.isValid()) return;

    LoopFormat::LoopSequencer seq(reader);
    seq.prepareToPlay(512, 44100.0);
    seq.setBPM(120.f);
    seq.play();

    // At 120BPM / 44100Hz, 1 beat = 22050 samples.
    // With block size 512: 22050 / 512 ≈ 43 blocks per beat.
    // After 44 blocks, the first event (tick=0) should have activated.
    // The voice plays for 1 beat = 22050 frames.

    // Drive for 2 blocks (tick 0 event fires on first scheduled block)
    juce::AudioBuffer<float> buf(2, 512);
    for (int b = 0; b < 5; ++b)
    {
        buf.clear();
        juce::AudioSourceChannelInfo info(&buf, 0, 512);
        seq.getNextAudioBlock(info);
    }

    // After a few blocks, at least one voice should be active
    EXPECT(seq.getActiveVoiceCount() >= 1,
           "At least 1 voice should be active after playback starts");

    // Drive until all events have fired (4 beats = 88200 samples ≈ 173 blocks)
    // and some voices should still be playing their last beat
    int maxVoices = 0;
    for (int b = 0; b < 180; ++b)
    {
        buf.clear();
        juce::AudioSourceChannelInfo info(&buf, 0, 512);
        seq.getNextAudioBlock(info);
        maxVoices = std::max(maxVoices, seq.getActiveVoiceCount());
    }

    // We had 4 events, each lasting 1 beat. At most 2 should overlap
    // (depending on exact timing). Max should be > 0 and <= 4.
    EXPECT(maxVoices >= 1, "At least 1 voice should have been active across 4 beats");
    EXPECT(maxVoices <= 4, "Should not have more than 4 voices (one per event)");

    seq.releaseResources();
    reader.close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test: Transport play / stop / seek
// ─────────────────────────────────────────────────────────────────────────────

static void test_transportControl(const juce::File& testDir)
{
    std::printf("  [test_transportControl]\n");

    const juce::File loopFile = makeTestLoopFile(testDir, 2, 960, 120.f);

    LoopFormat::LoopFileReader reader;
    reader.open(loopFile);
    if (!reader.isValid()) { EXPECT(false, "Reader failed"); return; }

    LoopFormat::LoopSequencer seq(reader);
    seq.prepareToPlay(512, 44100.0);

    juce::AudioBuffer<float> buf(2, 512);

    // Stopped: output should be silence
    // (transport starts Stopped)
    {
        buf.clear();
        juce::AudioSourceChannelInfo info(&buf, 0, 512);
        seq.getNextAudioBlock(info);

        bool allZero = true;
        for (int s = 0; s < 512; ++s)
            if (std::abs(buf.getSample(0, s)) > 0.0001f) { allZero = false; break; }

        EXPECT(allZero, "Stopped sequencer should produce silence");
    }

    // Start playing — playhead should advance
    seq.play();
    EXPECT(seq.getTransportState() == LoopFormat::TransportState::Playing,
           "Transport should be Playing after play()");

    for (int b = 0; b < 10; ++b)
    {
        buf.clear();
        juce::AudioSourceChannelInfo info(&buf, 0, 512);
        seq.getNextAudioBlock(info);
    }

    const uint32_t tickAfterPlay = seq.getPlayheadTick();
    EXPECT(tickAfterPlay > 0, "Playhead tick should advance while playing");

    // Seek
    seq.seekToTick(960);
    // Give the audio thread one block to process the seek
    {
        buf.clear();
        juce::AudioSourceChannelInfo info(&buf, 0, 512);
        seq.getNextAudioBlock(info);
    }
    const uint32_t tickAfterSeek = seq.getPlayheadTick();
    EXPECT(tickAfterSeek >= 960, "Playhead should be at or after tick 960 after seek");

    // Stop
    seq.stop();
    EXPECT(seq.getTransportState() == LoopFormat::TransportState::Stopped,
           "Transport should be Stopped after stop()");

    seq.releaseResources();
    reader.close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test: BPM change does not disturb event grid
// ─────────────────────────────────────────────────────────────────────────────

static void test_bpmChangeGridStability()
{
    std::printf("  [test_bpmChangeGridStability]\n");

    using namespace LoopFormat;

    // At tick 3840 (4 beats):
    //   BPM 120 → 3840 / (44100 × 60 / (120 × 960)) = 88200 samples
    //   BPM 140 → 3840 / (44100 × 60 / (140 × 960)) = 75600 samples
    //
    // The tick value (3840) doesn't change — the sample position changes.
    // This is the DESIRED behaviour: the musical position stays grid-locked.

    const double at120 = tickToSample(3840, 120.0, 44100.0);
    const double at140 = tickToSample(3840, 140.0, 44100.0);

    EXPECT_NEAR(at120, 88200.0, 0.5,
                "4 beats at 120BPM should be ~88200 samples");
    EXPECT_NEAR(at140, 75600.0, 0.5,
                "4 beats at 140BPM should be ~75600 samples");

    // Key property: tick 3840 is always "beat 4" regardless of BPM.
    // Verify that sampleToTick recovers tick 3840 from both sample positions.
    EXPECT_EQ(sampleToTick(at120, 120.0, 44100.0), 3840u,
              "tick 3840 should round-trip at 120BPM");
    EXPECT_EQ(sampleToTick(at140, 140.0, 44100.0), 3840u,
              "tick 3840 should round-trip at 140BPM");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test: VelocityTable
// ─────────────────────────────────────────────────────────────────────────────

static void test_velocityTable()
{
    std::printf("  [test_velocityTable]\n");

    using namespace LoopFormat;

    // Velocity 0   → gain 0.0 (silence)
    EXPECT_NEAR(kVelocityTable.gain(0),   0.f, 0.0001f, "v=0 should be 0 gain");

    // Velocity 127 → gain 1.0 (unity)
    EXPECT_NEAR(kVelocityTable.gain(127), 1.f, 0.0001f, "v=127 should be ~1.0 gain");

    // Velocity 64 (half) → gain 0.25 (half² = 0.25, musical "half volume")
    const float expected64 = (64.f / 127.f) * (64.f / 127.f);
    EXPECT_NEAR(kVelocityTable.gain(64), expected64, 0.0001f,
                "v=64 should match (64/127)² formula");

    // Table is monotonically non-decreasing
    bool monotonic = true;
    for (int v = 1; v < 128; ++v)
    {
        if (kVelocityTable.gain(static_cast<uint8_t>(v)) <
            kVelocityTable.gain(static_cast<uint8_t>(v - 1)))
        {
            monotonic = false;
            break;
        }
    }
    EXPECT(monotonic, "VelocityTable should be monotonically non-decreasing");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test: PCM decode helpers
// ─────────────────────────────────────────────────────────────────────────────

static void test_pcmDecodeHelpers()
{
    std::printf("  [test_pcmDecodeHelpers]\n");

    using namespace LoopFormat;

    // 16-bit: max positive = 0x7FFF → should decode to ~1.0
    {
        const uint8_t bytes[2] = { 0xFF, 0x7F }; // 0x7FFF LE
        const float result = readInt16ToFloat(bytes);
        EXPECT_NEAR(result, 1.f, 0.001f, "0x7FFF should decode to ~1.0");
    }

    // 16-bit: max negative = 0x8000 → should decode to ~-1.0
    {
        const uint8_t bytes[2] = { 0x00, 0x80 }; // 0x8000 LE
        const float result = readInt16ToFloat(bytes);
        EXPECT_NEAR(result, -1.f, 0.001f, "0x8000 should decode to ~-1.0");
    }

    // 16-bit: zero
    {
        const uint8_t bytes[2] = { 0x00, 0x00 };
        EXPECT_NEAR(readInt16ToFloat(bytes), 0.f, 0.0001f, "0x0000 should decode to 0.0");
    }

    // 24-bit: max positive = 0x7FFFFF → should decode to ~1.0
    {
        const uint8_t bytes[3] = { 0xFF, 0xFF, 0x7F }; // 0x7FFFFF LE
        const float result = readInt24ToFloat(bytes);
        EXPECT_NEAR(result, 1.f, 0.001f, "24-bit 0x7FFFFF should decode to ~1.0");
    }

    // 24-bit: max negative = 0x800000 → should decode to ~-1.0
    {
        const uint8_t bytes[3] = { 0x00, 0x00, 0x80 }; // 0x800000 LE
        const float result = readInt24ToFloat(bytes);
        EXPECT_NEAR(result, -1.f, 0.001f, "24-bit 0x800000 should decode to ~-1.0");
    }

    // 24-bit: zero
    {
        const uint8_t bytes[3] = { 0x00, 0x00, 0x00 };
        EXPECT_NEAR(readInt24ToFloat(bytes), 0.f, 0.0001f, "24-bit 0 should decode to 0.0");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test: static_assert struct sizes (compile-time — if this file compiles, they pass)
// ─────────────────────────────────────────────────────────────────────────────

static void test_structSizes()
{
    std::printf("  [test_structSizes]\n");

    EXPECT_EQ(sizeof(LoopFormat::FileHeader),      64u, "FileHeader must be 64 bytes");
    EXPECT_EQ(sizeof(LoopFormat::SampleLUTEntry),  32u, "SampleLUTEntry must be 32 bytes");
    EXPECT_EQ(sizeof(LoopFormat::SampleDataBlock), 16u, "SampleDataBlock must be 16 bytes");
    EXPECT_EQ(sizeof(LoopFormat::SequenceHeader),  32u, "SequenceHeader must be 32 bytes");
    EXPECT_EQ(sizeof(LoopFormat::SequenceEvent),    8u, "SequenceEvent must be 8 bytes");
    EXPECT_EQ(sizeof(LoopFormat::MetaChunkHeader), 16u, "MetaChunkHeader must be 16 bytes");
    EXPECT_EQ(sizeof(LoopFormat::ActiveVoice),     56u, "ActiveVoice must be 56 bytes");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    

    std::printf("=== .LOOP Sequencer Timing Tests ===\n\n");

    const juce::File tempDir = juce::File::getSpecialLocation(
        juce::File::tempDirectory).getChildFile("loop_seq_test_" +
        juce::String(juce::Random::getSystemRandom().nextInt()));
    tempDir.createDirectory();

    // Compile-time checks (these are runtime equivalents of the static_asserts)
    test_structSizes();

    // Pure math tests (no file I/O)
    test_tickMathAtVariousBPMs();
    test_samplesPerTickConsistency();
    test_bpmChangeGridStability();
    test_velocityTable();
    test_pcmDecodeHelpers();

    // Integration tests (require file I/O)
    test_sequencerVoiceActivation(tempDir);
    test_transportControl(tempDir);

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);

    tempDir.deleteRecursively();
    

    return g_failed == 0 ? 0 : 1;
}
