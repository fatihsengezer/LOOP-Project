/**
 * @file    LoopPlayerProcessor.h
 * @brief   JUCE AudioProcessor — the plugin/standalone entry point.
 *
 * =============================================================================
 * AGENT / DEVELOPER QUICK-START
 * =============================================================================
 *
 * PURPOSE
 *   LoopPlayerProcessor is the JUCE plugin shell that connects:
 *     - The host (DAW or standalone audio device) → audio I/O
 *     - The LoopFileReader                        → file data
 *     - The LoopSequencer                         → audio engine
 *     - The AudioProcessorValueTreeState (APVTS)  → automatable parameters
 *
 *   It does NOT implement a GUI — the editor class (LoopPlayerEditor, Phase 3)
 *   attaches separately via createEditor().
 *
 * JUCE PARAMETER TREE (APVTS)
 * ─────────────────────────────
 *   The following parameters are registered and automatable by the host:
 *
 *   ID                  | Type    | Range            | Default | Unit
 *   ──────────────────────────────────────────────────────────────────
 *   "bpm"               | Float   | [20, 400]        | 120     | BPM
 *   "master_gain"       | Float   | [0, 2]           | 1       | Linear
 *   "looping"           | Bool    | on/off           | on      | —
 *   "look_ahead_ticks"  | Int     | [1, 16]          | 2       | Ticks
 *
 *   Per-sample gain and mute would be Phase 3 additions. Stub IDs are reserved.
 *
 * FILE LOADING
 *   loadFile() is called from the message thread (UI button, drag-and-drop).
 *   It runs on a juce::Thread to avoid blocking the message thread.
 *   The audio thread is protected during the load by a std::atomic<bool>
 *   fileReady_ flag that gates the sequencer in processBlock().
 *
 * MIDI SUPPORT
 *   The processor accepts MIDI input. A MIDI note-on message overrides the
 *   velocity of the next SequenceEvent for the corresponding sample ID
 *   (sampleID = note number mod sampleCount). This allows live performance
 *   re-velocity without editing the sequence.
 *
 * PLUGIN FORMATS
 *   Configured in the Projucer / CMakeLists for: VST3, Standalone
 *   AU is possible on macOS; AAX requires an Avid developer account.
 *
 * PERSISTENCE (DAW SESSION SAVE/LOAD)
 *   getStateInformation() / setStateInformation() serialise:
 *     - The loaded file path (the .LOOP file is NOT embedded in the session)
 *     - All APVTS parameter values
 *   On load, the file is re-opened from the stored path.
 *
 * =============================================================================
 */

#pragma once

#include "LoopFileReader.h"
#include "LoopSequencer.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

namespace LoopFormat
{

class LoopPlayerProcessor  :  public juce::AudioProcessor,
                               public juce::AudioProcessorValueTreeState::Listener
{
public:
    // ── Construction ─────────────────────────────────────────────────────────
    LoopPlayerProcessor();
    ~LoopPlayerProcessor() override;

    // Not copyable or movable (AudioProcessor contract)
    LoopPlayerProcessor(const LoopPlayerProcessor&)            = delete;
    LoopPlayerProcessor& operator=(const LoopPlayerProcessor&) = delete;


    // ── File loading (message thread) ────────────────────────────────────────

    enum class LoadError
    {
        None = 0,
        FileNotFound,
        FormatError,        ///< LoopFileReader::Error wrapped here
        AudioDeviceNotReady,///< loadFile() called before prepareToPlay()
    };

    /**
     * @brief Load a `.LOOP` file asynchronously.
     *
     * Opens the file on the calling thread (blocking), but the audio thread
     * will not use the new data until after the next prepareToPlay() or
     * the next processBlock() boundary (guarded by fileReady_ atomic).
     *
     * Safe to call from the message thread at any time, including while
     * the audio device is running.
     *
     * @param file  The .LOOP file to load.
     * @return      LoadError::None on success.
     */
    [[nodiscard]] LoadError loadFile(const juce::File& file);

    /**
     * @brief Unload the current file and stop playback.
     * Safe to call while audio is running — uses the fileReady_ gate.
     */
    void unloadFile();

    [[nodiscard]] bool hasFileLoaded() const noexcept
    {
        return fileReady_.load(std::memory_order_acquire);
    }

    [[nodiscard]] juce::File getLoadedFile() const { return loadedFilePath_; }


    // ── Transport (message thread) ────────────────────────────────────────────

    void transportPlay()  { if (sequencer_) sequencer_->play();  }
    void transportStop()  { if (sequencer_) sequencer_->stop();  }
    void transportPause() { if (sequencer_) sequencer_->pause(); }
    void transportSeek(uint32_t tick) { if (sequencer_) sequencer_->seekToTick(tick); }


    // ── Accessors for the editor (message thread only) ────────────────────────

    /**
     * Returns a raw pointer to the live LoopSequencer.
     * May be nullptr if no file is loaded. Always check hasFileLoaded() first.
     * Message-thread only — do NOT call from the audio thread.
     */
    [[nodiscard]] LoopSequencer*   getSequencer() const noexcept { return sequencer_.get(); }

    /**
     * Returns a raw pointer to the live LoopFileReader.
     * May be nullptr if no file is loaded. Check isValid() before using.
     * Message-thread only.
     */
    [[nodiscard]] LoopFileReader*  getReader()    const noexcept { return reader_.get(); }


    // ── juce::AudioProcessor interface ───────────────────────────────────────

    const juce::String getName() const override { return "LOOP Player"; }

    /**
     * Called by the host before streaming begins (or when sample rate /
     * buffer size changes). Propagates to LoopSequencer::prepareToPlay().
     */
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;

    void releaseResources() override;

    /**
     * The main audio + MIDI callback.
     *
     * Steps:
     *   1. Process MIDI (velocity override for live performance)
     *   2. If fileReady_ is false: clear buffer and return
     *   3. Delegate to LoopSequencer::getNextAudioBlock()
     *
     * @param buffer       Audio I/O buffer (in-place processing).
     * @param midiMessages MIDI events from the host for this block.
     */
    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer&         midiMessages) override;

    // Required AudioProcessor overrides
    bool                        hasEditor()               const override { return true;  }
    juce::AudioProcessorEditor* createEditor()                  override;
    bool                        acceptsMidi()             const override { return true;  }
    bool                        producesMidi()            const override { return false; }
    bool                        isMidiEffect()            const override { return false; }
    double                      getTailLengthSeconds()    const override { return 0.0;   }
    int                         getNumPrograms()                override { return 1;     }
    int                         getCurrentProgram()             override { return 0;     }
    void setCurrentProgram(int)                                 override {}
    const juce::String getProgramName(int)                      override { return {};    }
    void changeProgramName(int, const juce::String&)            override {}

    /** Serialise state for DAW session save. */
    void getStateInformation(juce::MemoryBlock& destData) override;

    /** Restore state on DAW session load. */
    void setStateInformation(const void* data, int sizeInBytes) override;


    // ── APVTS parameter listener ──────────────────────────────────────────────

    /**
     * Called by APVTS whenever an automatable parameter changes.
     * Routes the change to the sequencer.
     */
    void parameterChanged(const juce::String& parameterID,
                          float newValue) override;


    // ── Parameter IDs (use these constants everywhere — no magic strings) ────

    static constexpr const char* kParamBPM           = "bpm";
    static constexpr const char* kParamMasterGain     = "master_gain";
    static constexpr const char* kParamLooping        = "looping";
    static constexpr const char* kParamLookAheadTicks = "look_ahead_ticks";

    /** The APVTS — expose publicly for the editor to attach sliders/knobs. */
    juce::AudioProcessorValueTreeState apvts;


private:
    // ─────────────────────────────────────────────────────────────────────────
    //  Private helpers
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Build the APVTS ParameterLayout.
     * Called once in the constructor via initialiser list.
     */
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /**
     * Process a MidiBuffer for this block.
     * Extracts note-on messages and stores velocity overrides in
     * midiVelocityOverride_ for the scheduler to pick up.
     *
     * @param midi  The MIDI buffer from processBlock().
     */
    void processMidi(const juce::MidiBuffer& midi) noexcept;


    // ─────────────────────────────────────────────────────────────────────────
    //  Owned objects
    // ─────────────────────────────────────────────────────────────────────────

    /// The file reader — opened in loadFile(), closed in unloadFile() / destructor.
    std::unique_ptr<LoopFileReader>  reader_;

    /// The sequencer engine — created after reader_ is valid.
    std::unique_ptr<LoopSequencer>   sequencer_;


    // ─────────────────────────────────────────────────────────────────────────
    //  Thread-safety state
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Set to true only after reader_ and sequencer_ are both valid AND
     * prepareToPlay() has been called. The audio thread checks this at
     * the top of processBlock() and bails if false.
     *
     * Write: message thread (after successful loadFile or before unloadFile)
     * Read:  audio thread (top of processBlock)
     */
    std::atomic<bool> fileReady_ { false };

    /// Last successfully loaded file path (persisted in session state).
    juce::File loadedFilePath_;

    /// Cached sample rate from prepareToPlay() — used in loadFile() to
    /// call sequencer_->prepareToPlay() without re-entering the audio thread.
    std::atomic<double> cachedSampleRate_    { 44100.0 };
    std::atomic<int>    cachedBlockSize_     { 512 };
    std::atomic<bool>   deviceIsPrepared_    { false };


    // ─────────────────────────────────────────────────────────────────────────
    //  MIDI state (lock-free — single audio thread reads)
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * Per-note velocity overrides set by incoming MIDI note-on messages.
     * Index = MIDI note number (0..127).
     * 0 = no override (use the SequenceEvent's velocity).
     *
     * Written by processMidi() (audio thread, start of processBlock).
     * The sequencer reads this when triggering voices.
     *
     * Using std::array<uint8_t, 128> rather than atomics — the whole array
     * is only ever read/written by the audio thread in this implementation.
     */
    std::array<uint8_t, 128> midiVelocityOverride_ {};

    JUCE_LEAK_DETECTOR(LoopPlayerProcessor)
};

} // namespace LoopFormat
