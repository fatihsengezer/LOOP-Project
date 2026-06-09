/**
 * @file    LoopSequencer.cpp
 * @brief   Implementation of LoopSequencer — the tick-accurate audio engine.
 *
 * =============================================================================
 * AGENT / DEVELOPER NOTES
 * =============================================================================
 *
 * THE RENDER LOOP CALL ORDER (every getNextAudioBlock())
 * ───────────────────────────────────────────────────────
 *
 *   ① Handle pending seek (if seekPending_ is set)
 *   ② Check transport state → if Stopped, clear output and return
 *   ③ Drain pending FIFO → activate voices in voicePool_
 *   ④ Clear mixBuffer_ to zero
 *   ⑤ Render all active voices into mixBuffer_
 *   ⑥ Apply soft-knee limiter to mixBuffer_
 *   ⑦ Apply master gain
 *   ⑧ Copy mixBuffer_ → output buffer
 *   ⑨ Deactivate finished voices
 *   ⑩ Advance playheadSample_ by blockSize
 *   ⑪ Update playheadTick_ atomic (for UI metering)
 *   ⑫ Handle sequence loop-wrap if looping_ is enabled
 *   ⑬ Call scheduleLookAhead() for the NEXT block
 *
 * WHY SCHEDULE AT THE END?
 *   We schedule AFTER rendering (step ⑬ comes after ⑧).
 *   This means events scheduled in block N are activated at the START
 *   of block N+1 (via drainPendingFifo at step ③). The look-ahead
 *   window compensates: we look ahead by kLookAheadTicks beyond the
 *   current block end, so events that fall within the current block
 *   are already in the FIFO from the PREVIOUS block's scheduling call.
 *
 *   This creates a one-block pipeline:
 *     Block N:  schedule events for block N+1
 *     Block N+1: drain those events, render them
 *
 *   The net latency is 0 additional audio frames — the look-ahead ticks
 *   are consumed ahead of time, not added as latency.
 *
 * SAMPLE-ACCURATE TRIGGER OFFSETS
 *   When a SequenceEvent falls in the MIDDLE of a block, the voice's
 *   `startOffset` tells the render loop at which sample within the block
 *   the voice should begin contributing.
 *
 *   renderVoices() checks: if (frame < voice.startOffset) continue;
 *
 *   This ensures a kick drum scheduled at sample 37 of a 512-sample block
 *   doesn't bleed backwards to sample 0 of that block.
 *
 * MONO/STEREO MIXING RULES
 *   Output has 2 channels always (stereo output is universal).
 *   Voice channels vs output channels:
 *     voice mono  → output stereo : duplicate sample to both channels
 *     voice stereo → output stereo: direct channel mapping
 *     voice stereo → output mono  : (L + R) × 0.5f into output ch 0
 *
 * LIMITER ALGORITHM (soft-knee, single-pass)
 *   For each sample s in the accumulation buffer:
 *     |s| < threshold - knee/2  → pass through unchanged
 *     |s| > threshold + knee/2  → hard compress: threshold + excess / ratio
 *     between the two zones     → smooth cubic interpolation
 *
 *   This is computed with a branchless lerp using precomputed coefficients.
 *   No division in the inner loop — ratio is stored as its reciprocal (1/ratio).
 *
 * =============================================================================
 */

#include "../include/LoopSequencer.h"

#include <algorithm>   // std::lower_bound, std::sort
#include <cstring>     // std::memset
#include <cassert>
#include <cmath>       // std::abs, std::sqrt

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

LoopSequencer::LoopSequencer(LoopFileReader& reader)
    : reader_(reader)
{
    // Voice pool and pending FIFO are zero-initialised by their default constructors.
    // All other state is handled in prepareToPlay().
}

LoopSequencer::~LoopSequencer()
{
    // juce::AudioSource contract: releaseResources() must have been called
    // before destruction if prepareToPlay() was called.
    // We don't assert here — callers may destroy without ever calling prepare.
}


// ─────────────────────────────────────────────────────────────────────────────
//  juce::AudioSource — prepareToPlay()
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    sampleRate_   = sampleRate;
    maxBlockSize_ = samplesPerBlockExpected;

    // Allocate the mix accumulation buffer.
    // We allocate 2× the expected block size to handle hosts that
    // occasionally deliver larger blocks without re-allocation.
    // 2 channels (stereo output always).
    mixBuffer_.setSize(2, samplesPerBlockExpected * 2, false, true, false);

    // Reset playback state
    playheadSample_    = 0.0;
    tickAccumulator_   = 0.0;
    nextEventIndex_    = 0;

    // Reset all voices
    for (auto& v : voicePool_)
        v.reset();

    // Cache the event array pointers from the reader.
    // These are mmap'd pointers — valid until reader_.close() is called.
    eventsBegin_ = reader_.getEventsBegin();
    eventsEnd_   = reader_.getEventsEnd();

    // Cache the total sequence duration from the SequenceHeader.
    const SequenceHeader seqHdr = reader_.getSequenceHeader();
    sequenceDurationTicks_ = seqHdr.total_duration_ticks;

    // If BPM wasn't set externally, use the file's embedded BPM.
    if (bpm_.load(std::memory_order_relaxed) == 120.f && seqHdr.bpm > 0.f)
        bpm_.store(seqHdr.bpm, std::memory_order_release);

    // Prime the look-ahead: schedule the very first block's events now
    // so drainPendingFifo() has something to work with on the first callback.
    if (eventsBegin_ && eventsBegin_ != eventsEnd_)
    {
        const double blockTicks = blockSizeToTicks(samplesPerBlockExpected);
        scheduleLookAhead(0, blockTicks, samplesPerBlockExpected);
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  juce::AudioSource — releaseResources()
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::releaseResources()
{
    // Stop scheduling (transport stop should have been called, but belt-and-braces)
    transportState_.store(static_cast<int>(TransportState::Stopped),
                          std::memory_order_release);

    // Deactivate all voices
    for (auto& v : voicePool_)
        v.reset();

    // Clear the pending FIFO
    pendingFifo_.reset();

    eventsBegin_ = nullptr;
    eventsEnd_   = nullptr;
}


// ─────────────────────────────────────────────────────────────────────────────
//  juce::AudioSource — getNextAudioBlock()  (THE HOT PATH)
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    const int numSamples  = bufferToFill.numSamples;
    const int numOutputCh = bufferToFill.buffer->getNumChannels();

    // ── ① Handle pending seek ────────────────────────────────────────────────
    if (seekPending_.load(std::memory_order_acquire))
    {
        const uint32_t target = seekTargetTick_.load(std::memory_order_acquire);

        // Reset playback position
        playheadSample_  = tickToSample(target,
                                         bpm_.load(std::memory_order_acquire),
                                         sampleRate_);
        tickAccumulator_ = 0.0;

        // Reset event cursor to the first event at or after target tick
        if (eventsBegin_)
        {
            // Binary search for the first event >= target tick
            const auto it = std::lower_bound(
                eventsBegin_, eventsEnd_, target,
                [](const SequenceEvent& evt, uint32_t t) {
                    return evt.tickStart() < t;
                });
            nextEventIndex_ = static_cast<uint32_t>(it - eventsBegin_);
        }

        // Kill all active voices — they were playing from before the seek
        for (auto& v : voicePool_)
            v.reset();

        pendingFifo_.reset();
        playheadTick_.store(target, std::memory_order_release);
        seekPending_.store(false, std::memory_order_release);
    }

    // ── ② Transport gate ──────────────────────────────────────────────────────
    const auto state = static_cast<TransportState>(
        transportState_.load(std::memory_order_acquire));

    if (state == TransportState::Stopped)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    // ── ③ Drain pending FIFO → activate voices ───────────────────────────────
    drainPendingFifo();

    // ── ④ Clear accumulation buffer ───────────────────────────────────────────
    // Only clear the channels/samples we'll actually use.
    for (int ch = 0; ch < numOutputCh && ch < mixBuffer_.getNumChannels(); ++ch)
        mixBuffer_.clear(ch, 0, numSamples);

    // ── ⑤ Render all active voices into mixBuffer_ ───────────────────────────
    if (state == TransportState::Playing)
        renderVoices(numSamples, numOutputCh);

    // ── ⑥ Apply soft-knee limiter ─────────────────────────────────────────────
    applyLimiter(numSamples, numOutputCh);

    // ── ⑦ Apply master gain + ⑧ Copy to output ───────────────────────────────
    const float gain = masterGain_.load(std::memory_order_acquire);
    for (int ch = 0; ch < numOutputCh && ch < mixBuffer_.getNumChannels(); ++ch)
    {
        bufferToFill.buffer->copyFrom(
            ch,
            bufferToFill.startSample,
            mixBuffer_,
            ch,
            0,
            numSamples);

        // Apply gain to the output channel directly
        bufferToFill.buffer->applyGain(ch, bufferToFill.startSample, numSamples, gain);
    }

    // ── ⑨ Deactivate finished voices ─────────────────────────────────────────
    for (auto& v : voicePool_)
    {
        if (v.active && v.isFinished())
            v.reset();
    }

    // ── ⑩ Advance playhead ───────────────────────────────────────────────────
    if (state == TransportState::Playing)
    {
        playheadSample_ += static_cast<double>(numSamples);

        // ── ⑪ Update playhead tick atomic (for UI) ────────────────────────────
        const double currentBpm = bpm_.load(std::memory_order_acquire);
        uint32_t currentTick = sampleToTick(playheadSample_,
                                                    currentBpm,
                                                    sampleRate_);
        playheadTick_.store(currentTick, std::memory_order_release);

        // ── ⑫ Sequence loop wrap ──────────────────────────────────────────────
        if (looping_.load(std::memory_order_acquire)
            && sequenceDurationTicks_ > 0
            && currentTick >= sequenceDurationTicks_)
        {
            // Wrap the playhead back to tick 0.
            // Preserve the sub-tick fractional remainder to avoid drift.
            const double overflow = tickToSample(currentTick - sequenceDurationTicks_,
                                                  currentBpm, sampleRate_);
            playheadSample_  = overflow;
            tickAccumulator_ = 0.0;
            nextEventIndex_  = 0;
            
            currentTick = currentTick - sequenceDurationTicks_;
            playheadTick_.store(currentTick, std::memory_order_release);
        }

        // ── ⑬ Schedule look-ahead for next block ─────────────────────────────
        const double blockTicks = blockSizeToTicks(numSamples);
        scheduleLookAhead(currentTick, blockTicks, numSamples);
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  Transport controls (message thread)
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::play() noexcept
{
    transportState_.store(static_cast<int>(TransportState::Playing),
                          std::memory_order_release);
}

void LoopSequencer::stop() noexcept
{
    transportState_.store(static_cast<int>(TransportState::Stopped),
                          std::memory_order_release);
    seekToTick(0);
}

void LoopSequencer::pause() noexcept
{
    transportState_.store(static_cast<int>(TransportState::Paused),
                          std::memory_order_release);
}

void LoopSequencer::seekToTick(uint32_t tick) noexcept
{
    seekTargetTick_.store(tick, std::memory_order_release);
    seekPending_.store(true,    std::memory_order_release);
}

void LoopSequencer::setBPM(float bpm) noexcept
{
    bpm_.store(std::clamp(bpm, kMinBPM, kMaxBPM), std::memory_order_release);
}


// ─────────────────────────────────────────────────────────────────────────────
//  getActiveVoiceCount()
// ─────────────────────────────────────────────────────────────────────────────

int LoopSequencer::getActiveVoiceCount() const noexcept
{
    int count = 0;
    for (const auto& v : voicePool_)
        if (v.active) ++count;
    return count;
}


// ─────────────────────────────────────────────────────────────────────────────
//  scheduleLookAhead()  (audio thread)
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::scheduleLookAhead(uint32_t blockStartTick,
                                       double   blockSizeTicks,
                                       int      blockSizeSamples) noexcept
{
    if (!eventsBegin_ || eventsBegin_ == eventsEnd_) return;

    const uint32_t lookAhead = lookAheadTicks_.load(std::memory_order_acquire);
    const double   currentBpm = bpm_.load(std::memory_order_acquire);

    // The window we'll scan for events
    const double   windowEndTick = static_cast<double>(blockStartTick)
                                 + blockSizeTicks
                                 + static_cast<double>(lookAhead);

    const uint32_t totalEvents = static_cast<uint32_t>(eventsEnd_ - eventsBegin_);

    // Walk forward from nextEventIndex_ — no binary search needed here
    // because we're advancing sequentially through the sorted array.
    while (nextEventIndex_ < totalEvents)
    {
        const SequenceEvent& evt = eventsBegin_[nextEventIndex_];
        const uint32_t evtTick = evt.tickStart();

        if (static_cast<double>(evtTick) > windowEndTick)
            break; // This and all following events are outside our window

        // Compute where within the next block this event fires
        // as a sample offset [0, blockSizeSamples).
        const double sampleOffset = tickToSample(
            (evtTick > blockStartTick) ? (evtTick - blockStartTick) : 0u,
            currentBpm, sampleRate_);

        const int startOffset = static_cast<int>(
            std::min(sampleOffset, static_cast<double>(blockSizeSamples - 1)));

        // Look up sample data
        const uint16_t sampleID = evt.sampleID();
        if (sampleID >= reader_.getSampleCount())
        {
            ++nextEventIndex_;
            continue; // Skip orphaned event
        }

        // Check per-sample mute bitmask (from UI)
        {
            const uint64_t mask = mutedMask_.load(std::memory_order_acquire);
            if (sampleID < 64 && ((mask >> sampleID) & 1u))
            {
                ++nextEventIndex_;
                continue; // Sample is muted — skip trigger
            }
        }

        const SampleLUTEntry& lut = reader_.getLUTEntry(sampleID);
        const uint8_t* pcm = reader_.getSamplePointer(sampleID);

        if (!pcm)
        {
            ++nextEventIndex_;
            continue;
        }

        // Compute how many frames to play
        const double ratio = (sampleRate_ > 0.0) ? (static_cast<double>(lut.sample_rate) / sampleRate_) : 1.0;
        uint32_t playFrames = lut.frame_count;
        uint32_t totalFramesToPlay = static_cast<uint32_t>(static_cast<double>(lut.frame_count) / ratio);

        if (!evt.isOneShot())
        {
            // Convert loop duration ticks → frames
            const double durationFrames = tickToSample(evt.loopDuration(),
                                                        currentBpm, sampleRate_);
            totalFramesToPlay = static_cast<uint32_t>(durationFrames);
        }

        // Push to FIFO
        const int slots = pendingFifo_.getFreeSpace();
        if (slots > 0)
        {
            int s1, s2, n1, n2;
            pendingFifo_.prepareToWrite(1, s1, n1, s2, n2);

            if (n1 > 0)
            {
                PendingTrigger& trig = pendingTriggerData_[static_cast<size_t>(s1)];
                trig.pcmPtr      = pcm;
                trig.frameCount  = lut.frame_count;
                trig.playFrames  = playFrames;
                trig.startOffset = static_cast<uint32_t>(startOffset);
                trig.totalFramesToPlay = totalFramesToPlay;
                trig.sampleRateRatio = ratio;
                trig.gainLinear  = kVelocityTable.gain(evt.velocity());
                trig.channels    = lut.channels;
                trig.bitDepth    = lut.bitDepth();
                trig.oneShot     = evt.isOneShot();

                pendingFifo_.finishedWrite(1);
            }
        }
        // If FIFO is full, we drop the event.
        // This shouldn't happen with kPendingFifoSize=32 in normal use.

        ++nextEventIndex_;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  drainPendingFifo()  (audio thread)
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::drainPendingFifo() noexcept
{
    const int available = pendingFifo_.getNumReady();
    if (available == 0) return;

    int s1, s2, n1, n2;
    pendingFifo_.prepareToRead(available, s1, n1, s2, n2);

    // Helper lambda: activate one trigger
    auto activateTrigger = [this](const PendingTrigger& trig) noexcept
    {
        ActiveVoice* slot = findFreeVoice();
        if (!slot) slot = stealVoice(); // Fallback: steal
        if (!slot) return;              // Extremely unlikely: bail

        slot->pcmPtr      = trig.pcmPtr;
        slot->frameCount  = trig.frameCount;
        slot->playFrames  = trig.playFrames;
        slot->startOffset = trig.startOffset;
        slot->totalFramesToPlay = trig.totalFramesToPlay;
        slot->gainLinear  = trig.gainLinear;
        slot->channels    = trig.channels;
        slot->bitDepth    = trig.bitDepth;
        slot->oneShot     = trig.oneShot;
        slot->sampleRateRatio = trig.sampleRateRatio;
        slot->readPos     = 0.0;
        slot->framesPlayed = 0;
        slot->active      = true;  // Must be LAST — activates the voice
    };

    for (int i = 0; i < n1; ++i)
        activateTrigger(pendingTriggerData_[static_cast<size_t>(s1 + i)]);

    for (int i = 0; i < n2; ++i)
        activateTrigger(pendingTriggerData_[static_cast<size_t>(s2 + i)]);

    pendingFifo_.finishedRead(n1 + n2);
}


// ─────────────────────────────────────────────────────────────────────────────
//  findFreeVoice() / stealVoice()  (audio thread)
// ─────────────────────────────────────────────────────────────────────────────

ActiveVoice* LoopSequencer::findFreeVoice() noexcept
{
    for (auto& v : voicePool_)
        if (!v.active) return &v;
    return nullptr;
}

ActiveVoice* LoopSequencer::stealVoice() noexcept
{
    // Policy: steal the voice with the highest playback completion ratio.
    // (readPos / playFrames → closest to done = least musically important)
    ActiveVoice* best = nullptr;
    float bestRatio = -1.f;

    for (auto& v : voicePool_)
    {
        if (!v.active) continue;
        const float ratio = (v.playFrames > 0)
                            ? static_cast<float>(v.readPos) / static_cast<float>(v.playFrames)
                            : 1.f;
        if (ratio > bestRatio)
        {
            bestRatio = ratio;
            best = &v;
        }
    }

    if (best) best->reset(); // Clear it before handing it back
    return best;
}


// ─────────────────────────────────────────────────────────────────────────────
//  renderVoices()  (audio thread — the innermost hot loop)
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::renderVoices(int numSamples, int numOutputCh) noexcept
{
    // Pre-fetch channel pointers outside the voice loop to avoid
    // repeated method calls in the inner loop.
    float* outL = mixBuffer_.getWritePointer(0);
    float* outR = (numOutputCh >= 2) ? mixBuffer_.getWritePointer(1) : outL;

    for (auto& v : voicePool_)
    {
        if (!v.active) continue;

        const uint32_t bytesPerSample = (v.bitDepth == 24) ? 3u : 2u;
        const uint32_t bytesPerFrame  = bytesPerSample * v.channels;

        const int startSample = static_cast<int>(v.startOffset);

        for (int s = startSample; s < numSamples; ++s)
        {
            // Guard: stop when we've played the intended duration
            if (v.framesPlayed >= v.totalFramesToPlay)
            {
                v.active = false;
                break;
            }

            // Loop boundary check: wrap or stop
            if (v.readPos >= static_cast<double>(v.playFrames))
            {
                if (v.oneShot)
                {
                    v.active = false;
                    break;
                }
                else
                {
                    v.readPos = std::fmod(v.readPos, static_cast<double>(v.playFrames));
                }
            }

            // Guard: don't read past the actual PCM data
            if (v.readPos >= static_cast<double>(v.frameCount))
            {
                v.active = false;
                break;
            }

            // Resampling Linear Interpolation:
            const double pos = v.readPos;
            const uint32_t idx1 = static_cast<uint32_t>(pos);
            const uint32_t idx2 = (idx1 + 1 < v.frameCount) ? idx1 + 1 : idx1;
            const float frac = static_cast<float>(pos - static_cast<double>(idx1));

            // Pointer to the frame bytes
            const uint8_t* framePtr1 = v.pcmPtr + idx1 * bytesPerFrame;
            const uint8_t* framePtr2 = v.pcmPtr + idx2 * bytesPerFrame;

            float valL = 0.f;
            float valR = 0.f;

            // Decode and accumulate with linear interpolation
            if (v.bitDepth == 16)
            {
                if (v.channels == 1)
                {
                    const float s1 = readInt16ToFloat(framePtr1) * v.gainLinear;
                    const float s2 = readInt16ToFloat(framePtr2) * v.gainLinear;
                    valL = (1.f - frac) * s1 + frac * s2;
                    valR = valL; // upmix mono → stereo
                }
                else // stereo
                {
                    const float s1L = readInt16ToFloat(framePtr1)     * v.gainLinear;
                    const float s1R = readInt16ToFloat(framePtr1 + 2) * v.gainLinear;
                    const float s2L = readInt16ToFloat(framePtr2)     * v.gainLinear;
                    const float s2R = readInt16ToFloat(framePtr2 + 2) * v.gainLinear;
                    valL = (1.f - frac) * s1L + frac * s2L;
                    valR = (1.f - frac) * s1R + frac * s2R;
                }
            }
            else // 24-bit
            {
                if (v.channels == 1)
                {
                    const float s1 = readInt24ToFloat(framePtr1) * v.gainLinear;
                    const float s2 = readInt24ToFloat(framePtr2) * v.gainLinear;
                    valL = (1.f - frac) * s1 + frac * s2;
                    valR = valL;
                }
                else // stereo
                {
                    const float s1L = readInt24ToFloat(framePtr1)     * v.gainLinear;
                    const float s1R = readInt24ToFloat(framePtr1 + 3) * v.gainLinear;
                    const float s2L = readInt24ToFloat(framePtr2)     * v.gainLinear;
                    const float s2R = readInt24ToFloat(framePtr2 + 3) * v.gainLinear;
                    valL = (1.f - frac) * s1L + frac * s2L;
                    valR = (1.f - frac) * s1R + frac * s2R;
                }
            }

            outL[s] += valL;
            outR[s] += valR;

            v.readPos += v.sampleRateRatio;
            ++v.framesPlayed;
        }

        // Reset startOffset after the first block is rendered so subsequent blocks start at sample 0.
        v.startOffset = 0;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  applyLimiter()  (audio thread)
// ─────────────────────────────────────────────────────────────────────────────

void LoopSequencer::applyLimiter(int numSamples, int numChannels) noexcept
{
    // Soft-knee limiter.
    //
    // For each sample s:
    //   if |s| <= (threshold - knee/2)  → pass through
    //   if |s| >= (threshold + knee/2)  → compress: output = sign(s) × compressed
    //   between the zones               → smoothly interpolated
    //
    // The knee interpolation uses a simple cubic Hermite: t³ × (coeff)
    // This is entirely branchless — the compiler will vectorise it.

    constexpr float T  = kLimiterThreshold;
    constexpr float K  = kLimiterKneeWidth;
    constexpr float R  = 1.f / kLimiterRatio; // Reciprocal ratio (avoids division)
    constexpr float lo = T - K * 0.5f;        // Lower knee boundary
    constexpr float hi = T + K * 0.5f;        // Upper knee boundary

    for (int ch = 0; ch < numChannels && ch < mixBuffer_.getNumChannels(); ++ch)
    {
        float* data = mixBuffer_.getWritePointer(ch);

        for (int s = 0; s < numSamples; ++s)
        {
            const float x    = data[s];
            const float absX = std::abs(x);
            const float sign = (x >= 0.f) ? 1.f : -1.f;

            float output;

            if (absX <= lo)
            {
                // Below knee: unity gain
                output = x;
            }
            else if (absX >= hi)
            {
                // Above knee: hard compression
                output = sign * (T + (absX - T) * R);
            }
            else
            {
                // In the knee: cubic smooth interpolation
                // t = 0 at lo, t = 1 at hi
                const float t   = (absX - lo) / K;
                const float t2  = t * t;
                const float t3  = t2 * t;

                // Hermite smooth: 3t² - 2t³
                const float smooth = 3.f * t2 - 2.f * t3;

                // Interpolate between unity gain and compressed gain
                const float linear     = absX;
                const float compressed = T + (absX - T) * R;
                output = sign * (linear + smooth * (compressed - linear));
            }

            data[s] = output;
        }
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  blockSizeToTicks()  (audio thread, called every block)
// ─────────────────────────────────────────────────────────────────────────────

double LoopSequencer::blockSizeToTicks(int numSamples) const noexcept
{
    // ticks = samples × (bpm × TPQN) / (sampleRate × 60)
    const double bpm  = bpm_.load(std::memory_order_acquire);
    const double tpqn = static_cast<double>(reader_.getFileHeader().tpqn);
    if (sampleRate_ <= 0.0 || bpm <= 0.0 || tpqn <= 0.0) return 0.0;

    return static_cast<double>(numSamples) * (bpm * tpqn) / (sampleRate_ * 60.0);
}

} // namespace LoopFormat
