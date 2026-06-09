/**
 * @file    LoopCreatorView.h
 * @brief   A user-friendly Creator Mode GUI to load WAV files, arrange them on a grid, and compile .loop files.
 */

#pragma once

#include "LoopPlayerProcessor.h"
#include "FormatSpec.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <vector>
#include <map>
#include <string>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  CreatorData structures
// ─────────────────────────────────────────────────────────────────────────────

struct CreatorSample
{
    uint16_t id = 0;
    juce::File file;
    juce::String name;
    double sampleRate = 44100.0;
    int channels = 1;
    int bitDepth = 16;
    float baseNote = 60.f;
    float loopBpm = 0.f;
    juce::AudioBuffer<float> buffer;
};

struct CreatorEvent
{
    uint32_t tickStart = 0;
    uint16_t sampleId = 0;
    uint8_t velocity = 100;
    uint32_t duration = 960; // default 1 beat (960 ticks)
    bool oneShot = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineEditorComponent — Custom component for drawing & interacting with the grid
// ─────────────────────────────────────────────────────────────────────────────

class TimelineEditorComponent : public juce::Component
{
public:
    TimelineEditorComponent(const std::vector<CreatorSample>& samples,
                            std::vector<CreatorEvent>& events);

    void updateData() { repaint(); }

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // Callbacks to notify parent view of changes
    std::function<void()> onTimelineChanged;

    static constexpr int kRowHeight = 36;
    static constexpr int kColWidth = 30; // 16th note step width
    static constexpr int kLabelWidth = 80;
    static constexpr int kHeaderHeight = 24;
    static constexpr int kNumSteps = 64; // 16 beats * 4 steps = 64 steps

private:
    const std::vector<CreatorSample>& samples_;
    std::vector<CreatorEvent>& events_;

    // Interaction state
    int draggingEventIdx_ = -1;
    bool isResizing_ = false;
    uint32_t dragStartTick_ = 0;
    uint32_t dragStartDuration_ = 0;
    juce::Point<int> dragStartPos_;

    // Helper functions
    int getEventIndexAt(juce::Point<int> pos) const;
    bool isOverEventRightEdge(juce::Point<int> pos, int eventIdx) const;
    void getGridCoords(juce::Point<int> pos, int& row, int& step) const;
    uint32_t stepToTick(int step) const { return static_cast<uint32_t>(step) * 240; } // 240 ticks per 16th note step
    int tickToStep(uint32_t tick) const { return static_cast<int>(tick) / 240; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  LoopCreatorView
// ─────────────────────────────────────────────────────────────────────────────

class LoopCreatorView : public juce::Component,
                        public juce::ListBoxModel
{
public:
    LoopCreatorView(LoopPlayerProcessor& processor, std::function<void(const juce::File&)> onPreviewLoadRequest);
    ~LoopCreatorView() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // ListBoxModel interface for sample list
    int getNumRows() override { return static_cast<int>(samples_.size()); }
    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

private:
    void addSample();
    void removeSelectedSample();
    void compileAndExport();
    void triggerPreview();
    void stopPreview();

    // Data compilation helper
    bool compileTo(const juce::File& file);

    LoopPlayerProcessor& processor_;
    std::function<void(const juce::File&)> onPreviewLoadRequest_;

    std::vector<CreatorSample> samples_;
    std::vector<CreatorEvent> events_;

    // GUI Layout
    juce::GroupComponent sampleGroup_  { "samples_group", "Sample Pool" };
    juce::ListBox        sampleListBox_;
    juce::TextButton     addSampleBtn_ { "+" };
    juce::TextButton     removeSampleBtn_ { "-" };

    juce::GroupComponent timelineGroup_ { "timeline_group", "Timeline Grid (64 steps / 16 beats)" };
    juce::Viewport       timelineViewport_;
    std::unique_ptr<TimelineEditorComponent> timelineEditor_;

    juce::GroupComponent settingsGroup_ { "settings_group", "Export Settings" };
    juce::Label          projTitleLabel_ { "proj_title_lbl", "Project Name:" };
    juce::TextEditor     projTitleEdit_;
    juce::Label          authorLabel_    { "author_lbl", "Author:" };
    juce::TextEditor     authorEdit_;
    juce::Label          bpmLabel_       { "bpm_lbl", "Tempo (BPM):" };
    juce::Slider         bpmSlider_;

    juce::TextButton     previewBtn_     { "Play Preview" };
    juce::TextButton     exportBtn_      { "Export .LOOP File" };

    juce::AudioFormatManager formatManager_;
    std::unique_ptr<juce::FileChooser> fileChooser_;

    bool isPlayingPreview_ = false;
    juce::File tempPreviewFile_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoopCreatorView)
};

} // namespace LoopFormat
