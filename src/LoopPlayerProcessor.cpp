/**
 * @file    LoopPlayerProcessor.cpp
 * @brief   Implementation of LoopPlayerProcessor — the JUCE plugin entry point.
 *
 * =============================================================================
 * AGENT / DEVELOPER NOTES
 * =============================================================================
 *
 * OBJECT LIFECYCLE
 * ────────────────
 *   Message thread:
 *     ctor() → loadFile() → [host calls prepareToPlay()] → loadFile() (again ok)
 *     ↕ (session running, user may call loadFile() anytime)
 *     [host calls releaseResources()] → unloadFile() → dtor()
 *
 *   Audio thread:
 *     prepareToPlay() → processBlock() × N → releaseResources()
 *
 * THE fileReady_ GATE
 * ────────────────────
 *   loadFile() does:
 *     1. fileReady_ = false          (audio thread stops using old reader/sequencer)
 *     2. Destroy old sequencer + reader
 *     3. Construct new reader, open file
 *     4. Construct new sequencer
 *     5. If device is prepared: call sequencer->prepareToPlay(cachedSampleRate_, ...)
 *     6. fileReady_ = true           (audio thread starts using new objects)
 *
 *   The risk: between steps 1 and 6, the audio thread calls processBlock() with
 *   fileReady_=false and just clears the buffer. This is correct behaviour —
 *   a brief silence during file load is acceptable.
 *
 * APVTS LISTENER PATTERN
 *   The processor registers itself as an APVTS::Listener for all four parameters.
 *   parameterChanged() runs on the message thread and writes to the sequencer's
 *   atomics. This is the correct JUCE pattern for plugin automation.
 *
 * SESSION STATE PERSISTENCE
 *   We store a juce::ValueTree as XML with two children:
 *     <PARAMETERS>  → from apvts.copyState()
 *     <FILE path="..."/>  → the loaded .LOOP file path
 *
 *   On setStateInformation(), we restore the APVTS state first (which
 *   triggers parameterChanged callbacks), then reload the file.
 *   File-not-found is handled gracefully (just no audio, no crash).
 *
 * =============================================================================
 */

#include "../include/LoopPlayerProcessor.h"

#include "../include/LoopPlayerEditor.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Parameter layout
// ─────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorValueTreeState::ParameterLayout
LoopPlayerProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // BPM — [20, 400], default 120, step 0.01
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { kParamBPM, 1 },
        "BPM",
        juce::NormalisableRange<float>(20.f, 400.f, 0.01f, 0.5f), // skew 0.5 = log-ish
        120.f,
        juce::AudioParameterFloatAttributes()
            .withLabel("BPM")
            .withStringFromValueFunction([](float v, int) {
                return juce::String(v, 2);
            })));

    // Master Gain — [0, 2], default 1.0
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { kParamMasterGain, 1 },
        "Master Gain",
        juce::NormalisableRange<float>(0.f, 2.f, 0.001f),
        1.f,
        juce::AudioParameterFloatAttributes().withLabel("x")));

    // Looping — bool, default on
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { kParamLooping, 1 },
        "Loop",
        true));

    // Look-ahead ticks — int [1, 16], default 2
    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { kParamLookAheadTicks, 1 },
        "Look-Ahead Ticks",
        1, 16, 2));

    return layout;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

LoopPlayerProcessor::LoopPlayerProcessor()
    : AudioProcessor(BusesProperties()
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , apvts(*this, nullptr, "LOOP_STATE", createParameterLayout())
{
    logDebug("LoopPlayerProcessor ctor start");
    // Register as APVTS listener for all parameters
    apvts.addParameterListener(kParamBPM,           this);
    apvts.addParameterListener(kParamMasterGain,     this);
    apvts.addParameterListener(kParamLooping,        this);
    apvts.addParameterListener(kParamLookAheadTicks, this);

    // Initialise velocity override array to 0 (no override)
    midiVelocityOverride_.fill(0);
    logDebug("LoopPlayerProcessor ctor done");
}

LoopPlayerProcessor::~LoopPlayerProcessor()
{
    apvts.removeParameterListener(kParamBPM,           this);
    apvts.removeParameterListener(kParamMasterGain,     this);
    apvts.removeParameterListener(kParamLooping,        this);
    apvts.removeParameterListener(kParamLookAheadTicks, this);

    // Ensure audio thread is done before destroying objects
    unloadFile();
}


// ─────────────────────────────────────────────────────────────────────────────
//  File loading / unloading
// ─────────────────────────────────────────────────────────────────────────────

LoopPlayerProcessor::LoadError LoopPlayerProcessor::loadFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return LoadError::FileNotFound;

    // ── Step 1: Gate the audio thread ────────────────────────────────────────
    fileReady_.store(false, std::memory_order_release);

    // Give the audio thread one block to notice fileReady_=false.
    // In a production app, use a juce::SpinLock or a flag-and-wait pattern.
    // Here we use a small sleep — acceptable during file load.
    juce::Thread::sleep(5); // 5ms >> one 512-sample block at 44100Hz

    // ── Step 2: Destroy old objects ──────────────────────────────────────────
    if (sequencer_)
    {
        sequencer_->releaseResources();
        sequencer_.reset();
    }
    if (reader_)
    {
        reader_->close();
        reader_.reset();
    }

    // ── Step 3: Open new reader ───────────────────────────────────────────────
    auto newReader = std::make_unique<LoopFileReader>();
    const auto readerErr = newReader->open(file);

    if (readerErr != LoopFileReader::Error::None)
    {
        // LoadError maps to the reader error (both are "format error" to the UI)
        return LoadError::FormatError;
    }

    // ── Step 4: Create sequencer ──────────────────────────────────────────────
    auto newSequencer = std::make_unique<LoopSequencer>(*newReader);

    // ── Step 5: Apply current APVTS parameters ────────────────────────────────
    if (auto* p = apvts.getRawParameterValue(kParamBPM))
        newSequencer->setBPM(p->load());

    if (auto* p = apvts.getRawParameterValue(kParamMasterGain))
        newSequencer->setMasterGain(p->load());

    if (auto* p = apvts.getRawParameterValue(kParamLooping))
        newSequencer->setLooping(p->load() > 0.5f);

    if (auto* p = apvts.getRawParameterValue(kParamLookAheadTicks))
        newSequencer->setLookAheadTicks(static_cast<uint32_t>(p->load()));

    // ── Step 5b: Call prepareToPlay if the device is already running ──────────
    if (deviceIsPrepared_.load(std::memory_order_acquire))
    {
        const double sr = cachedSampleRate_.load(std::memory_order_acquire);
        const int    bs = cachedBlockSize_.load(std::memory_order_acquire);
        newSequencer->prepareToPlay(bs, sr);
    }

    // ── Step 6: Commit and open the gate ─────────────────────────────────────
    reader_        = std::move(newReader);
    sequencer_     = std::move(newSequencer);
    loadedFilePath_ = file;

    fileReady_.store(true, std::memory_order_release);

    return LoadError::None;
}

void LoopPlayerProcessor::unloadFile()
{
    fileReady_.store(false, std::memory_order_release);
    juce::Thread::sleep(5); // Same pattern as loadFile

    if (sequencer_) { sequencer_->releaseResources(); sequencer_.reset(); }
    if (reader_)    { reader_->close(); reader_.reset(); }

    loadedFilePath_ = {};
}


// ─────────────────────────────────────────────────────────────────────────────
//  prepareToPlay()
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerProcessor::prepareToPlay(double sampleRate,
                                         int    maximumExpectedSamplesPerBlock)
{
    logDebug("LoopPlayerProcessor::prepareToPlay sr=" + std::to_string(sampleRate) + " bs=" + std::to_string(maximumExpectedSamplesPerBlock));
    cachedSampleRate_.store(sampleRate,                     std::memory_order_release);
    cachedBlockSize_.store(maximumExpectedSamplesPerBlock,  std::memory_order_release);
    deviceIsPrepared_.store(true,                           std::memory_order_release);

    if (sequencer_ && fileReady_.load(std::memory_order_acquire))
    {
        logDebug("LoopPlayerProcessor::prepareToPlay: preparing sequencer");
        sequencer_->prepareToPlay(maximumExpectedSamplesPerBlock, sampleRate);
    }
    logDebug("LoopPlayerProcessor::prepareToPlay done");
}


// ─────────────────────────────────────────────────────────────────────────────
//  releaseResources()
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerProcessor::releaseResources()
{
    deviceIsPrepared_.store(false, std::memory_order_release);
    if (sequencer_) sequencer_->releaseResources();
}


// ─────────────────────────────────────────────────────────────────────────────
//  processBlock()  (audio thread — HOT PATH)
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer&         midiMessages)
{
    juce::ScopedNoDenormals noDenormals; // Flush denormals — common JUCE pattern

    // ── Gate: no file loaded or not prepared ─────────────────────────────────
    if (!fileReady_.load(std::memory_order_acquire))
    {
        buffer.clear();
        return;
    }

    // ── Process MIDI (before sequencer call) ─────────────────────────────────
    processMidi(midiMessages);
    midiMessages.clear(); // We consume MIDI; don't pass through

    // ── Delegate to sequencer ─────────────────────────────────────────────────
    // Wrap the AudioBuffer in a juce::AudioSourceChannelInfo as required
    // by getNextAudioBlock().
    juce::AudioSourceChannelInfo info (&buffer, 0, buffer.getNumSamples());
    sequencer_->getNextAudioBlock(info);
}


// ─────────────────────────────────────────────────────────────────────────────
//  processMidi()
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerProcessor::processMidi(const juce::MidiBuffer& midi) noexcept
{
    // Scan for note-on messages. Map note number → velocity override.
    // The sequencer currently uses these overrides when it triggers voices
    // for events with matching sampleID = noteNumber % sampleCount.
    //
    // NOTE: This is a simple "last note wins" policy for the block.
    // A full implementation would pass the MIDI timestamps through to the
    // sequencer for sample-accurate MIDI triggering.

    for (const juce::MidiMessageMetadata m : midi)
    {
        const auto msg = m.getMessage();
        if (msg.isNoteOn())
        {
            const int  note = msg.getNoteNumber();   // 0..127
            const uint8_t vel = static_cast<uint8_t>(msg.getVelocity()); // 0..127
            midiVelocityOverride_[static_cast<size_t>(note)] = vel;
        }
        else if (msg.isNoteOff())
        {
            midiVelocityOverride_[static_cast<size_t>(msg.getNoteNumber())] = 0;
        }
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  parameterChanged() — APVTS listener (message thread)
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerProcessor::parameterChanged(const juce::String& parameterID,
                                            float               newValue)
{
    if (!sequencer_) return;

    if (parameterID == kParamBPM)
        sequencer_->setBPM(newValue);

    else if (parameterID == kParamMasterGain)
        sequencer_->setMasterGain(newValue);

    else if (parameterID == kParamLooping)
        sequencer_->setLooping(newValue > 0.5f);

    else if (parameterID == kParamLookAheadTicks)
        sequencer_->setLookAheadTicks(static_cast<uint32_t>(newValue));
}


// ─────────────────────────────────────────────────────────────────────────────
//  Session state persistence
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree state("LOOP_PLUGIN_STATE");

    // Store APVTS parameter values
    state.addChild(apvts.copyState(), -1, nullptr);

    // Store the loaded file path
    if (loadedFilePath_ != juce::File{})
    {
        juce::ValueTree fileNode("FILE");
        fileNode.setProperty("path", loadedFilePath_.getFullPathName(), nullptr);
        state.addChild(fileNode, -1, nullptr);
    }

    // Serialise to binary XML
    juce::MemoryOutputStream stream(destData, true);
    state.writeToStream(stream);
}

void LoopPlayerProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const juce::ValueTree state = juce::ValueTree::readFromData(data,
                                      static_cast<size_t>(sizeInBytes));

    if (!state.isValid()) return;

    // Restore APVTS parameters (triggers parameterChanged callbacks)
    const juce::ValueTree paramsChild = state.getChildWithName(apvts.state.getType());
    if (paramsChild.isValid())
        apvts.replaceState(paramsChild);

    // Reload the file from the stored path
    const juce::ValueTree fileNode = state.getChildWithName("FILE");
    if (fileNode.isValid())
    {
        const juce::String path = fileNode.getProperty("path", "");
        if (path.isNotEmpty())
        {
            const juce::File f(path);
            if (f.existsAsFile())
                (void)loadFile(f); // Returns LoadError — silently ignored on restore
                                   // (file may have moved; UI should show a re-link prompt)
        }
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  createEditor()  — Phase 3
// ─────────────────────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* LoopPlayerProcessor::createEditor()
{
    return new LoopPlayerEditor(*this);
}


// ─────────────────────────────────────────────────────────────────────────────
//  JUCE plugin factory function (required boilerplate)
// ─────────────────────────────────────────────────────────────────────────────

} // namespace LoopFormat

// This function must be in the global namespace for the JUCE plugin scanner.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoopFormat::LoopPlayerProcessor();
}
