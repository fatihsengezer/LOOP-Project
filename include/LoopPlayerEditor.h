/**
 * @file    LoopPlayerEditor.h
 * @brief   JUCE AudioProcessorEditor — Phase 3 custom GUI for the LOOP Player.
 *
 * =============================================================================
 * ARCHITECTURE OVERVIEW
 * =============================================================================
 *
 *  LoopPlayerEditor  (resizable, 900×560 default, 700×420 minimum)
 *  ├── HeaderBar area    — logo, file name label, Browse button
 *  ├── BeatGridComponent — scrolling event grid + animated playhead
 *  ├── TransportPanel    — Play/Pause/Stop + BPM rotary + Loop toggle
 *  ├── VoiceMeterComponent — LED-dot polyphony display
 *  └── StatusBar area    — file info text strip
 *
 * INTERACTIVITY
 *  - Left-click on a beat-grid event  → seek playhead to that event's tick
 *  - Right-click on a beat-grid event → toggle per-sample-ID mute
 *  - Drag a .loop file onto the window → load it
 *  - Browse button → native file picker
 *  - Play / Pause / Stop buttons → transport control
 *  - BPM rotary knob → APVTS-attached, automatable from host
 *  - Loop toggle → APVTS-attached
 *
 * THREADING
 *  The editor lives on the message thread.
 *  A 30-Hz juce::Timer polls:
 *    - sequencer->getPlayheadTick()    → drives playhead animation
 *    - sequencer->getActiveVoiceCount()→ drives voice meter
 *  The timer callback is the ONLY place we read audio-side state.
 *
 * =============================================================================
 */

#pragma once

#include "LoopPlayerProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <set>
#include <vector>
#include <functional>
#include <cstdint>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Colour constants
// ─────────────────────────────────────────────────────────────────────────────

namespace EditorColors
{
    static const juce::Colour Background    { 0xFF0D1117 };
    static const juce::Colour Surface       { 0xFF161B22 };
    static const juce::Colour SurfaceRaised { 0xFF1C2128 };
    static const juce::Colour Border        { 0xFF30363D };
    static const juce::Colour Accent        { 0xFF00D4FF };
    static const juce::Colour AccentGlow    { 0x3300D4FF };
    static const juce::Colour TextPrimary   { 0xFFC9D1D9 };
    static const juce::Colour TextSecondary { 0xFF8B949E };
    static const juce::Colour Danger        { 0xFFE85C50 };
    static const juce::Colour PlayGreen     { 0xFF3FB950 };
}

// 8-colour palette for sample IDs in the beat grid
inline const juce::Colour kSamplePalette[8] = {
    juce::Colour { 0xFF4E9AF1 },  // blue
    juce::Colour { 0xFFE85C50 },  // red
    juce::Colour { 0xFF50C878 },  // green
    juce::Colour { 0xFFFFB347 },  // orange
    juce::Colour { 0xFF9B59B6 },  // purple
    juce::Colour { 0xFF1ABC9C },  // teal
    juce::Colour { 0xFFFF79C6 },  // pink
    juce::Colour { 0xFFFFD700 },  // gold
};

inline juce::Colour sampleColor(uint16_t sampleId) noexcept
{
    return kSamplePalette[sampleId % 8];
}


// ─────────────────────────────────────────────────────────────────────────────
//  LoopLookAndFeel
// ─────────────────────────────────────────────────────────────────────────────

class LoopLookAndFeel : public juce::LookAndFeel_V4
{
public:
    LoopLookAndFeel();

    void drawRotarySlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override;

    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool isHighlighted,
                              bool isDown) override;

    void drawToggleButton(juce::Graphics& g,
                          juce::ToggleButton& button,
                          bool isHighlighted,
                          bool isDown) override;

    juce::Font getLabelFont(juce::Label& label) override;
    juce::Font getTextButtonFont(juce::TextButton& btn, int buttonHeight) override;

private:
    juce::Font primaryFont_;
};


// ─────────────────────────────────────────────────────────────────────────────
//  CachedEvent  (message-thread copy of SequenceEvent data)
// ─────────────────────────────────────────────────────────────────────────────

struct CachedEvent
{
    uint32_t tickStart     = 0;
    uint32_t durationTicks = 0;   ///< loopDuration (may equal frame_count if oneShot)
    uint16_t sampleId      = 0;
    uint8_t  velocity      = 100;
    bool     isOneShot     = false;
};


// ─────────────────────────────────────────────────────────────────────────────
//  BeatGridComponent
// ─────────────────────────────────────────────────────────────────────────────

class BeatGridComponent : public juce::Component
{
public:
    BeatGridComponent();

    // ── Data ─────────────────────────────────────────────────────────────────

    /** Populate the grid from a freshly-loaded file. */
    void setEvents(std::vector<CachedEvent> events,
                   uint32_t totalDurationTicks,
                   uint32_t tpqn);

    /** Clear to empty-state (no file loaded). */
    void clearEvents();

    // ── Playback state ────────────────────────────────────────────────────────

    /** Update playhead — called by editor's timerCallback(). */
    void setPlayheadTick(uint32_t tick);

    // ── Mute state ────────────────────────────────────────────────────────────

    /** Toggle mute for a sample ID. Returns new mute state. */
    bool toggleMuteSampleId(uint16_t sampleId);

    /** Query mute state. */
    bool isSampleMuted(uint16_t sampleId) const noexcept;

    /** Copy the current muted-ID set (for syncing to the sequencer). */
    const std::set<uint16_t>& getMutedIds() const noexcept { return mutedSampleIds_; }

    // ── Callbacks ─────────────────────────────────────────────────────────────

    /** Fires when the user left-clicks an event. Arg = event tick. */
    std::function<void(uint32_t tick)> onSeekRequested;

    /** Fires when the user right-clicks an event. Args = sampleId, newMuted. */
    std::function<void(uint16_t sampleId, bool muted)> onMuteToggled;

    // ── juce::Component ──────────────────────────────────────────────────────

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

private:
    // ── Layout helpers ────────────────────────────────────────────────────────
    float tickToX(uint32_t tick) const noexcept;
    float sampleIdToY(uint16_t id) const noexcept;
    juce::Rectangle<float> eventRect(const CachedEvent& e) const noexcept;

    // ── Hit testing ───────────────────────────────────────────────────────────
    int hoveredEventIndex(juce::Point<float> pos) const noexcept; ///< -1 = none

    // ── Grid drawing helpers ──────────────────────────────────────────────────
    void drawBackground(juce::Graphics& g) const;
    void drawBeatLines(juce::Graphics& g) const;
    void drawEvents(juce::Graphics& g) const;
    void drawPlayhead(juce::Graphics& g) const;
    void drawEmptyState(juce::Graphics& g) const;
    void drawRowLabels(juce::Graphics& g) const;

    // ── Data ──────────────────────────────────────────────────────────────────
    std::vector<CachedEvent> events_;
    std::set<uint16_t>       mutedSampleIds_;
    std::vector<uint16_t>    orderedRows_; ///< unique sampleIds in appearance order

    uint32_t totalDurationTicks_ = 0;
    uint32_t tpqn_               = 960;
    uint32_t playheadTick_       = 0;
    int      hoveredIdx_         = -1;

    // ── Layout ────────────────────────────────────────────────────────────────
    static constexpr float kLabelW  = 52.f;  ///< left column for row labels
    static constexpr float kHeaderH = 22.f;  ///< top row for beat numbers
    static constexpr float kRowH    = 28.f;  ///< height of each sample row

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BeatGridComponent)
};


// ─────────────────────────────────────────────────────────────────────────────
//  VoiceMeterComponent
// ─────────────────────────────────────────────────────────────────────────────

class VoiceMeterComponent : public juce::Component
{
public:
    VoiceMeterComponent();

    /** Update active voice count — called by editor's timerCallback(). */
    void setActiveVoiceCount(int count) noexcept;

    void paint(juce::Graphics& g) override;

    static constexpr int kTotalDots = 16; ///< visual polyphony slots shown

private:
    int   activeCount_  = 0;
    float smoothed_     = 0.f;  ///< decaying float for animation

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceMeterComponent)
};


// ─────────────────────────────────────────────────────────────────────────────
//  LoopPlayerEditor
// ─────────────────────────────────────────────────────────────────────────────

class LoopPlayerEditor : public juce::AudioProcessorEditor,
                          public juce::FileDragAndDropTarget,
                          public juce::Timer
{
public:
    explicit LoopPlayerEditor(LoopPlayerProcessor& processor);
    ~LoopPlayerEditor() override;

    // ── juce::AudioProcessorEditor ───────────────────────────────────────────
    void paint(juce::Graphics& g) override;
    void resized() override;

    // ── juce::FileDragAndDropTarget ───────────────────────────────────────────
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // ── juce::Timer ──────────────────────────────────────────────────────────
    void timerCallback() override;  ///< 30 Hz; polls sequencer state

private:
    // ── Setup helpers ─────────────────────────────────────────────────────────
    void buildComponents();
    void buildTransportButtons();
    void buildBpmKnob();
    void connectApvts();

    // ── File operations (message thread) ─────────────────────────────────────
    void tryLoadFile(const juce::File& file);
    void launchFileBrowser();
    void refreshSequenceData();      ///< reads events from reader into beat grid
    void updateFileLabel();
    void syncMutesToSequencer();     ///< push muted-ID bitmask to sequencer

    // ── State ─────────────────────────────────────────────────────────────────
    LoopPlayerProcessor& processor_;
    LoopLookAndFeel      laf_;       ///< must outlive all child components

    // ── Sub-components ────────────────────────────────────────────────────────
    BeatGridComponent*   beatGrid_   = nullptr;
    VoiceMeterComponent* voiceMeter_ = nullptr;

    // Header widgets
    juce::Label      logoLabel_;
    juce::Label      fileLabel_;
    juce::TextButton browseButton_ { "Browse\xe2\x80\xa6" }; // "Browse…"

    // Transport widgets
    juce::TextButton playButton_  { "\xe2\x96\xb6" };   // ▶
    juce::TextButton pauseButton_ { "\xe2\x8f\xb8" };   // ⏸
    juce::TextButton stopButton_  { "\xe2\x96\xa0" };   // ■

    juce::ToggleButton loopButton_ { "LOOP" };

    // BPM rotary
    juce::Slider bpmKnob_;
    juce::Label  bpmTitleLabel_;

    // Status strip
    juce::Label statusLabel_;

    // APVTS attachments (must outlive knob/button)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bpmAttachment_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttachment_;

    // File chooser (kept alive across async callback)
    std::unique_ptr<juce::FileChooser> fileChooser_;

    // Drag overlay
    bool isDraggingFile_  = false;

    // Previous-state cache for efficient repaints
    bool     hadFileLastTick_   = false;
    uint32_t lastPlayheadTick_  = UINT32_MAX;
    int      lastVoiceCount_    = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopPlayerEditor)
};

} // namespace LoopFormat
