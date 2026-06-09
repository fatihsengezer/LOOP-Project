/**
 * @file    LoopCreatorView.cpp
 * @brief   Implementation of LoopCreatorView — Creator Mode GUI.
 */

#include "../include/LoopCreatorView.h"
#include "../include/LoopFileWriter.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Local color palette (aligned with LoopPlayerEditor)
// ─────────────────────────────────────────────────────────────────────────────

namespace CreatorColors
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

// Reuse sample palette for colors of events on grid
static const juce::Colour kCreatorSamplePalette[8] = {
    juce::Colour { 0xFF4E9AF1 },  // blue
    juce::Colour { 0xFFE85C50 },  // red
    juce::Colour { 0xFF50C878 },  // green
    juce::Colour { 0xFFFFB347 },  // orange
    juce::Colour { 0xFF9B59B6 },  // purple
    juce::Colour { 0xFF1ABC9C },  // teal
    juce::Colour { 0xFFFF79C6 },  // pink
    juce::Colour { 0xFFFFD700 },  // gold
};

static juce::Colour creatorSampleColor (uint16_t sampleId) noexcept
{
    return kCreatorSamplePalette[sampleId % 8];
}


// ═════════════════════════════════════════════════════════════════════════════
//  TimelineEditorComponent Implementation
// ═════════════════════════════════════════════════════════════════════════════

TimelineEditorComponent::TimelineEditorComponent(const std::vector<CreatorSample>& samples,
                                                 std::vector<CreatorEvent>& events)
    : samples_(samples)
    , events_(events)
{
    setOpaque(true);
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void TimelineEditorComponent::paint(juce::Graphics& g)
{
    g.fillAll(CreatorColors::Background);

    const int totalHeight = getHeight();
    const int totalWidth = getWidth();

    // ── 1. Draw Grid Lines & Backgrounds ─────────────────────────────────────
    
    // Draw row backgrounds
    for (size_t r = 0; r < samples_.size(); ++r)
    {
        const int y = kHeaderHeight + static_cast<int>(r) * kRowHeight;
        g.setColour((r % 2 == 0) ? CreatorColors::Background 
                                  : CreatorColors::Surface.withAlpha(0.4f));
        g.fillRect(0, y, totalWidth, kRowHeight);
    }

    // Left label column background
    g.setColour(CreatorColors::Surface);
    g.fillRect(0, 0, kLabelWidth, totalHeight);

    // Separator line for label column
    g.setColour(CreatorColors::Border);
    g.drawVerticalLine(kLabelWidth, 0.f, static_cast<float>(totalHeight));

    // Draw grid columns
    for (int step = 0; step <= kNumSteps; ++step)
    {
        const int x = kLabelWidth + step * kColWidth;
        const bool isBar = (step % 16 == 0);
        const bool isBeat = (step % 4 == 0);

        if (isBar)
        {
            g.setColour(juce::Colour(0xFF444C56));
            g.drawVerticalLine(x, 0.f, static_cast<float>(totalHeight));
        }
        else if (isBeat)
        {
            g.setColour(juce::Colour(0xFF2D333B));
            g.drawVerticalLine(x, kHeaderHeight, static_cast<float>(totalHeight));
        }
        else
        {
            g.setColour(juce::Colour(0xFF21262D));
            g.drawVerticalLine(x, kHeaderHeight, static_cast<float>(totalHeight));
        }
    }

    // Draw horizontal row dividers
    for (size_t r = 0; r <= samples_.size(); ++r)
    {
        const int y = kHeaderHeight + static_cast<int>(r) * kRowHeight;
        g.setColour(CreatorColors::Border);
        g.drawHorizontalLine(y, 0.f, static_cast<float>(totalWidth));
    }

    // ── 2. Draw Headers & Row Labels ─────────────────────────────────────────
    
    // Header background
    g.setColour(CreatorColors::SurfaceRaised);
    g.fillRect(0, 0, totalWidth, kHeaderHeight);
    g.setColour(CreatorColors::Border);
    g.drawHorizontalLine(kHeaderHeight, 0.f, static_cast<float>(totalWidth));

    // Beat numbers
    g.setFont(juce::Font(10.f));
    g.setColour(CreatorColors::TextSecondary);
    for (int beat = 0; beat < kNumSteps / 4; ++beat)
    {
        const int x = kLabelWidth + beat * 4 * kColWidth;
        g.drawText(juce::String(beat + 1), x + 4, 0, 40, kHeaderHeight, 
                   juce::Justification::centredLeft, false);
    }

    // Track labels
    g.setFont(juce::Font(11.f));
    for (size_t r = 0; r < samples_.size(); ++r)
    {
        const auto& s = samples_[r];
        const int y = kHeaderHeight + static_cast<int>(r) * kRowHeight;

        g.setColour(creatorSampleColor(s.id));
        g.fillRoundedRectangle(6.f, static_cast<float>(y + kRowHeight / 2 - 4), 8.f, 8.f, 2.f);

        g.setColour(CreatorColors::TextPrimary);
        g.drawText("S" + juce::String(s.id) + " - " + s.name.substring(0, 10),
                   20, y, kLabelWidth - 24, kRowHeight,
                   juce::Justification::centredLeft, true);
    }

    // Empty state help text
    if (samples_.empty())
    {
        g.setColour(CreatorColors::TextSecondary.withAlpha(0.5f));
        g.setFont(juce::Font(13.f));
        g.drawText("Add a sample to populate tracks", 
                   kLabelWidth, 0, totalWidth - kLabelWidth, totalHeight,
                   juce::Justification::centred, false);
        return;
    }

    // ── 3. Draw Events ───────────────────────────────────────────────────────
    for (size_t i = 0; i < events_.size(); ++i)
    {
        const auto& e = events_[i];
        
        // Find row index for sample ID
        auto rowIt = std::find_if(samples_.begin(), samples_.end(), 
                                  [&e](const CreatorSample& s) { return s.id == e.sampleId; });
        if (rowIt == samples_.end())
            continue; // Sample no longer exists
        
        const int rIdx = static_cast<int>(std::distance(samples_.begin(), rowIt));
        const int y = kHeaderHeight + rIdx * kRowHeight;
        
        const int startStep = tickToStep(e.tickStart);
        const int durationSteps = tickToStep(e.duration);

        const int x = kLabelWidth + startStep * kColWidth;
        const int w = std::max(durationSteps * kColWidth, 4);

        juce::Rectangle<int> rect (x + 2, y + 2, w - 4, kRowHeight - 4);
        
        juce::Colour col = creatorSampleColor(e.sampleId);

        // Fill
        juce::ColourGradient grad (
            col.brighter(0.2f), static_cast<float>(rect.getX()), static_cast<float>(rect.getY()),
            col.darker(0.3f),   static_cast<float>(rect.getX()), static_cast<float>(rect.getBottom()),
            false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(rect.toFloat(), 3.f);

        // Border
        const bool isHovered = (draggingEventIdx_ == static_cast<int>(i));
        g.setColour(isHovered ? CreatorColors::Accent : col.brighter(0.5f));
        g.drawRoundedRectangle(rect.toFloat(), 3.f, isHovered ? 1.5f : 1.f);

        // Velocity text
        g.setColour(juce::Colours::white.withAlpha(0.8f));
        g.setFont(juce::Font(9.f));
        g.drawText("v" + juce::String(e.velocity), rect.reduced(4, 2), 
                   juce::Justification::topLeft, false);

        // One-shot / Loop badge
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.setFont(juce::Font(8.f));
        g.drawText(e.oneShot ? "1x" : "loop", rect.reduced(4, 2),
                   juce::Justification::bottomRight, false);
    }
}

int TimelineEditorComponent::getEventIndexAt(juce::Point<int> pos) const
{
    for (int i = static_cast<int>(events_.size()) - 1; i >= 0; --i)
    {
        const auto& e = events_[static_cast<size_t>(i)];
        
        auto rowIt = std::find_if(samples_.begin(), samples_.end(), 
                                  [&e](const CreatorSample& s) { return s.id == e.sampleId; });
        if (rowIt == samples_.end())
            continue;

        const int rIdx = static_cast<int>(std::distance(samples_.begin(), rowIt));
        const int y = kHeaderHeight + rIdx * kRowHeight;
        
        const int startStep = tickToStep(e.tickStart);
        const int durationSteps = tickToStep(e.duration);

        const int x = kLabelWidth + startStep * kColWidth;
        const int w = std::max(durationSteps * kColWidth, 4);

        juce::Rectangle<int> rect (x, y, w, kRowHeight);
        if (rect.contains(pos))
            return i;
    }
    return -1;
}

bool TimelineEditorComponent::isOverEventRightEdge(juce::Point<int> pos, int eventIdx) const
{
    if (eventIdx < 0 || eventIdx >= static_cast<int>(events_.size()))
        return false;

    const auto& e = events_[static_cast<size_t>(eventIdx)];
    
    auto rowIt = std::find_if(samples_.begin(), samples_.end(), 
                              [&e](const CreatorSample& s) { return s.id == e.sampleId; });
    if (rowIt == samples_.end())
        return false;

    const int startStep = tickToStep(e.tickStart);
    const int durationSteps = tickToStep(e.duration);
    
    const int x = kLabelWidth + startStep * kColWidth;
    const int w = std::max(durationSteps * kColWidth, 4);
    const int rightX = x + w;

    return std::abs(pos.getX() - rightX) <= 6;
}

void TimelineEditorComponent::getGridCoords(juce::Point<int> pos, int& row, int& step) const
{
    row = (pos.getY() - kHeaderHeight) / kRowHeight;
    step = (pos.getX() - kLabelWidth) / kColWidth;
    
    row = std::clamp(row, 0, std::max(0, static_cast<int>(samples_.size()) - 1));
    step = std::clamp(step, 0, kNumSteps - 1);
}

void TimelineEditorComponent::mouseDown(const juce::MouseEvent& e)
{
    if (samples_.empty()) return;

    const auto pos = e.position.toInt();

    // ── Handle Right-click: Delete Event ─────────────────────────────────────
    if (e.mods.isRightButtonDown())
    {
        const int idx = getEventIndexAt(pos);
        if (idx >= 0)
        {
            events_.erase(events_.begin() + idx);
            repaint();
            if (onTimelineChanged)
                onTimelineChanged();
        }
        return;
    }

    // ── Handle Left-click: Select / Create Event ─────────────────────────────
    const int idx = getEventIndexAt(pos);
    if (idx >= 0)
    {
        draggingEventIdx_ = idx;
        dragStartPos_ = pos;
        dragStartTick_ = events_[static_cast<size_t>(idx)].tickStart;
        dragStartDuration_ = events_[static_cast<size_t>(idx)].duration;
        isResizing_ = isOverEventRightEdge(pos, idx);
        
        setMouseCursor(isResizing_ ? juce::MouseCursor::LeftRightResizeCursor 
                                   : juce::MouseCursor::DraggingHandCursor);
        repaint();
    }
    else
    {
        // Clicked empty cell → create a new event (1 beat = 960 ticks = 4 steps)
        int row = 0, step = 0;
        getGridCoords(pos, row, step);

        if (pos.getX() >= kLabelWidth)
        {
            CreatorEvent newEvt;
            newEvt.tickStart = stepToTick(step);
            newEvt.sampleId = samples_[static_cast<size_t>(row)].id;
            newEvt.duration = 960; // default 1 beat
            newEvt.velocity = 100;
            newEvt.oneShot = true;

            events_.push_back(newEvt);
            
            // Auto start drag on the newly created event
            draggingEventIdx_ = static_cast<int>(events_.size()) - 1;
            dragStartPos_ = pos;
            dragStartTick_ = newEvt.tickStart;
            dragStartDuration_ = newEvt.duration;
            isResizing_ = false;

            repaint();
            if (onTimelineChanged)
                onTimelineChanged();
        }
    }
}

void TimelineEditorComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (draggingEventIdx_ < 0 || draggingEventIdx_ >= static_cast<int>(events_.size()))
        return;

    const auto pos = e.position.toInt();
    auto& evt = events_[static_cast<size_t>(draggingEventIdx_)];

    const int deltaX = pos.getX() - dragStartPos_.getX();
    const int deltaSteps = deltaX / kColWidth;

    if (isResizing_)
    {
        // Resize duration
        int newSteps = static_cast<int>(tickToStep(dragStartDuration_)) + deltaSteps;
        newSteps = std::clamp(newSteps, 1, kNumSteps);
        evt.duration = stepToTick(newSteps);
    }
    else
    {
        // Move event position
        int newStep = static_cast<int>(tickToStep(dragStartTick_)) / 240 + deltaSteps;
        newStep = std::clamp(newStep, 0, kNumSteps - 1);
        evt.tickStart = stepToTick(newStep);

        // Drag vertically to change sample tracks (if dragged enough)
        int rowIdx = 0, stepIdx = 0;
        getGridCoords(pos, rowIdx, stepIdx);
        if (rowIdx >= 0 && rowIdx < static_cast<int>(samples_.size()))
        {
            evt.sampleId = samples_[static_cast<size_t>(rowIdx)].id;
        }
    }

    repaint();
}

void TimelineEditorComponent::mouseUp(const juce::MouseEvent&)
{
    draggingEventIdx_ = -1;
    isResizing_ = false;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
    
    if (onTimelineChanged)
        onTimelineChanged();
}


// ═════════════════════════════════════════════════════════════════════════════
//  LoopCreatorView Implementation
// ═════════════════════════════════════════════════════════════════════════════

LoopCreatorView::LoopCreatorView(LoopPlayerProcessor& processor,
                                 std::function<void(const juce::File&)> onPreviewLoadRequest)
    : processor_(processor)
    , onPreviewLoadRequest_(onPreviewLoadRequest)
{
    logDebug("LoopCreatorView ctor start");
    // Setup formats
    formatManager_.registerBasicFormats();

    // Setup GUI Elements
    addAndMakeVisible(sampleGroup_);
    
    addAndMakeVisible(sampleListBox_);
    sampleListBox_.setModel(this);
    sampleListBox_.setColour(juce::ListBox::backgroundColourId, CreatorColors::Surface);
    sampleListBox_.setColour(juce::ListBox::outlineColourId, CreatorColors::Border);
    sampleListBox_.setOutlineThickness(1);

    addAndMakeVisible(addSampleBtn_);
    addSampleBtn_.onClick = [this] { addSample(); };

    addAndMakeVisible(removeSampleBtn_);
    removeSampleBtn_.onClick = [this] { removeSelectedSample(); };

    addAndMakeVisible(timelineGroup_);
    addAndMakeVisible(timelineViewport_);

    // Timeline component setup
    logDebug("LoopCreatorView ctor: creating TimelineEditorComponent");
    timelineEditor_ = std::make_unique<TimelineEditorComponent>(samples_, events_);
    timelineEditor_->onTimelineChanged = [this] { triggerPreview(); };
    timelineViewport_.setViewedComponent(timelineEditor_.get(), false);
    timelineViewport_.setScrollBarsShown(true, true);

    addAndMakeVisible(settingsGroup_);
    
    addAndMakeVisible(projTitleLabel_);
    projTitleEdit_.setText("My Loop Project");
    addAndMakeVisible(projTitleEdit_);

    addAndMakeVisible(authorLabel_);
    authorEdit_.setText("Author");
    addAndMakeVisible(authorEdit_);

    addAndMakeVisible(bpmLabel_);
    bpmSlider_.setSliderStyle(juce::Slider::LinearBar);
    bpmSlider_.setRange(20.0, 400.0, 1.0);
    bpmSlider_.setValue(120.0);
    bpmSlider_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 60, 20);
    bpmSlider_.onValueChange = [this] { triggerPreview(); };
    addAndMakeVisible(bpmSlider_);

    addAndMakeVisible(previewBtn_);
    previewBtn_.onClick = [this] { 
        if (isPlayingPreview_) 
            stopPreview();
        else 
            triggerPreview(); 
    };

    addAndMakeVisible(exportBtn_);
    exportBtn_.onClick = [this] { compileAndExport(); };

    // Setup temp preview path
    tempPreviewFile_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("loop_editor_preview.loop");
    logDebug("LoopCreatorView ctor done");
}

LoopCreatorView::~LoopCreatorView()
{
    if (isPlayingPreview_)
        stopPreview();

    // Delete temp file if exists
    if (tempPreviewFile_.existsAsFile())
        tempPreviewFile_.deleteFile();
}

void LoopCreatorView::resized()
{
    logDebug("LoopCreatorView::resized start");
    const int margin = 12;
    const int w = getWidth();
    const int h = getHeight();

    // ── 1. Layout Left Panel (Sample Pool) ───────────────────────────────────
    const int leftW = 220;
    const int leftH = h - margin * 2;
    sampleGroup_.setBounds(margin, margin, leftW, leftH);

    const int listY = margin + 26;
    const int btnH = 26;
    const int listH = leftH - 26 - btnH - margin * 2;
    sampleListBox_.setBounds(margin + 10, listY, leftW - 20, listH);

    const int btnY = listY + listH + 8;
    addSampleBtn_.setBounds(margin + 10, btnY, (leftW - 30) / 2, btnH);
    removeSampleBtn_.setBounds(margin + 10 + (leftW - 30) / 2 + 10, btnY, (leftW - 30) / 2, btnH);

    // ── 2. Layout Settings & Export (Bottom Panel) ───────────────────────────
    const int rightX = margin * 2 + leftW;
    const int rightW = w - rightX - margin;
    const int botH = 100;
    const int botY = h - botH - margin;
    settingsGroup_.setBounds(rightX, botY, rightW, botH);

    const int lblW = 90;
    const int editW = 140;
    const int editH = 22;
    
    // Rows
    projTitleLabel_.setBounds(rightX + 16, botY + 28, lblW, editH);
    projTitleEdit_.setBounds(rightX + 16 + lblW, botY + 28, editW, editH);
    
    authorLabel_.setBounds(rightX + 16, botY + 28 + 28, lblW, editH);
    authorEdit_.setBounds(rightX + 16 + lblW, botY + 28 + 28, editW, editH);

    const int bpmX = rightX + 16 + lblW + editW + 30;
    bpmLabel_.setBounds(bpmX, botY + 28, 120, editH);
    bpmSlider_.setBounds(bpmX, botY + 28 + 24, 180, editH);

    const int exportBtnW = 140;
    const int exportX = rightX + rightW - exportBtnW - 16;
    previewBtn_.setBounds(exportX, botY + 24, exportBtnW, btnH);
    exportBtn_.setBounds(exportX, botY + 24 + 32, exportBtnW, btnH);

    // ── 3. Layout Timeline Grid (Top Right) ──────────────────────────────────
    const int gridH = h - botH - margin * 3 - 12;
    timelineGroup_.setBounds(rightX, margin, rightW, gridH);
    timelineViewport_.setBounds(rightX + 10, margin + 22, rightW - 20, gridH - 32);

    // Update internal size of Timeline editor component based on rows
    const int contentW = TimelineEditorComponent::kLabelWidth + TimelineEditorComponent::kNumSteps * TimelineEditorComponent::kColWidth;
    const int contentH = TimelineEditorComponent::kHeaderHeight + static_cast<int>(samples_.size()) * TimelineEditorComponent::kRowHeight;
    timelineEditor_->setSize(std::max(contentW, timelineViewport_.getWidth()), std::max(contentH, timelineViewport_.getHeight() - 16));
}

void LoopCreatorView::paint(juce::Graphics& g)
{
    g.fillAll(CreatorColors::Background);
}

void LoopCreatorView::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool isSelected)
{
    if (row < 0 || row >= static_cast<int>(samples_.size())) return;
    const auto& s = samples_[static_cast<size_t>(row)];

    if (isSelected)
    {
        g.setColour(CreatorColors::Accent.withAlpha(0.18f));
        g.fillAll();
        g.setColour(CreatorColors::Accent);
        g.drawRect(0, 0, width, height, 1);
    }

    g.setColour(CreatorColors::TextPrimary);
    g.setFont(juce::Font(12.f));
    
    // Sample Name
    g.drawText("S" + juce::String(s.id) + ": " + s.name, 8, 4, width - 16, height / 2,
               juce::Justification::bottomLeft, true);

    // Detail strip
    g.setColour(CreatorColors::TextSecondary);
    g.setFont(juce::Font(10.f));
    juce::String details = juce::String(s.sampleRate, 0) + "Hz  |  " 
                          + juce::String(s.bitDepth) + "-bit  |  "
                          + (s.channels == 2 ? "Stereo" : "Mono");
    g.drawText(details, 8, height / 2 + 2, width - 16, height / 2 - 4,
               juce::Justification::topLeft, true);
}

void LoopCreatorView::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    // Double click -> prompt to edit sample metadata (base note / loop BPM)
    if (row < 0 || row >= static_cast<int>(samples_.size())) return;
    const size_t sampleIdx = static_cast<size_t>(row);
    const auto& s = samples_[sampleIdx];

    auto alert = std::make_shared<juce::AlertWindow> ("Edit Sample Properties", "Modify parameters for " + s.name, juce::AlertWindow::QuestionIcon, this);
    alert->addTextEditor ("note", juce::String(s.baseNote), "Base MIDI Note (default 60):");
    alert->addTextEditor ("bpm", juce::String(s.loopBpm), "Original Loop BPM (0 = trigger one-shot):");
    alert->addButton ("OK", 1);
    alert->addButton ("Cancel", 0);

    alert->enterModalState (true, juce::ModalCallbackFunction::create ([this, alert, sampleIdx] (int result)
    {
        if (result == 1 && sampleIdx < samples_.size())
        {
            auto& sample = samples_[sampleIdx];
            sample.baseNote = std::clamp(alert->getTextEditorContents("note").getFloatValue(), 0.f, 127.f);
            sample.loopBpm  = std::clamp(alert->getTextEditorContents("bpm").getFloatValue(), 0.f, 400.f);
            sampleListBox_.updateContent();
            triggerPreview();
        }
    }), false);
}

void LoopCreatorView::addSample()
{
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Load WAV Sample",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.wav;*.aif;*.aiff",
        true);

    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (! file.existsAsFile()) return;

            std::unique_ptr<juce::AudioFormatReader> reader (formatManager_.createReaderFor (file));
            if (reader == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Format Error", 
                    "Unsupported file format. Please select a valid WAV or AIFF file.", "OK", this);
                return;
            }

            // Verify sample rate matches existing samples (enforce spec invariant)
            if (! samples_.empty())
            {
                if (std::abs(reader->sampleRate - samples_[0].sampleRate) > 0.01)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Sample Rate Mismatch", 
                        "The sample rate (" + juce::String(reader->sampleRate) + "Hz) must match existing samples ("
                        + juce::String(samples_[0].sampleRate) + "Hz) to compile correctly.", "OK", this);
                    return;
                }
            }

            CreatorSample s;
            s.id = static_cast<uint16_t>(samples_.size());
            s.file = file;
            s.name = file.getFileNameWithoutExtension();
            s.sampleRate = reader->sampleRate;
            s.channels = static_cast<int>(reader->numChannels);
            s.bitDepth = static_cast<int>(reader->bitsPerSample);
            s.baseNote = 60.f;
            s.loopBpm = 0.f; // default to trigger once

            s.buffer.setSize(s.channels, static_cast<int>(reader->lengthInSamples));
            reader->read(&s.buffer, 0, static_cast<int>(reader->lengthInSamples), 0, true, true);

            samples_.push_back(s);
            
            sampleListBox_.updateContent();
            resized(); // force viewport refresh
            
            triggerPreview();
        });
}

void LoopCreatorView::removeSelectedSample()
{
    const int idx = sampleListBox_.getSelectedRow();
    if (idx < 0 || idx >= static_cast<int>(samples_.size())) return;

    const uint16_t removedId = samples_[static_cast<size_t>(idx)].id;

    samples_.erase(samples_.begin() + idx);

    // Re-index remaining samples to maintain consecutive IDs [0..N)
    for (size_t i = 0; i < samples_.size(); ++i)
        samples_[i].id = static_cast<uint16_t>(i);

    // Delete events referencing the removed sample or re-map IDs
    events_.erase(
        std::remove_if(events_.begin(), events_.end(), 
                       [removedId](const CreatorEvent& e) { return e.sampleId == removedId; }),
        events_.end());

    // Re-map event sampleIds to match the new indices
    for (auto& e : events_)
    {
        if (e.sampleId > removedId && e.sampleId > 0)
            e.sampleId--;
    }

    sampleListBox_.updateContent();
    resized();
    triggerPreview();
}

bool LoopCreatorView::compileTo(const juce::File& file)
{
    if (samples_.empty()) return false;

    LoopFileWriter writer;

    // Time signature and metadata
    writer.setTimeSignature (4, 4);
    writer.addMeta ("project", projTitleEdit_.getText().toStdString());
    writer.addMeta ("author", authorEdit_.getText().toStdString());

    // Add samples
    for (const auto& s : samples_)
    {
        SampleMeta meta;
        meta.sample_rate = static_cast<uint32_t>(s.sampleRate);
        meta.channels    = static_cast<uint8_t>(s.channels);
        meta.bit_depth   = static_cast<uint8_t>(s.bitDepth == 24 ? 24 : 16);
        meta.base_note   = s.baseNote;
        meta.loop_bpm    = s.loopBpm;
        meta.name        = s.name.toStdString();

        uint16_t assigned = 0;
        auto err = writer.addSample(s.buffer, meta, &assigned);
        if (err != LoopFileWriter::Error::None)
        {
            std::cerr << "Compile error adding sample: " << LoopFileWriter::describeError(err) << "\n";
            return false;
        }
    }

    // Add events
    for (const auto& e : events_)
    {
        auto err = writer.addEvent(e.tickStart, e.sampleId, e.velocity, e.duration, e.oneShot);
        if (err != LoopFileWriter::Error::None)
        {
            std::cerr << "Compile error adding event: " << LoopFileWriter::describeError(err) << "\n";
            return false;
        }
    }

    auto writeErr = writer.write(file);
    return (writeErr == LoopFileWriter::Error::None);
}

void LoopCreatorView::triggerPreview()
{
    if (samples_.empty()) return;

    // Compile timeline configuration to temporary preview file
    if (! compileTo (tempPreviewFile_))
        return;

    // Set preview state
    isPlayingPreview_ = true;
    previewBtn_.setButtonText ("Stop Preview");
    previewBtn_.setColour (juce::TextButton::buttonColourId, CreatorColors::Danger);

    // Call callback to load the temp loop file into the processor and start playing
    if (onPreviewLoadRequest_)
        onPreviewLoadRequest_ (tempPreviewFile_);

    // Set project settings BPM to match slider
    processor_.apvts.getParameter (LoopPlayerProcessor::kParamBPM)
        ->setValueNotifyingHost (processor_.apvts.getParameterRange (LoopPlayerProcessor::kParamBPM)
                                     .convertTo0to1 (static_cast<float> (bpmSlider_.getValue())));

    // Trigger Play
    processor_.transportPlay();
}

void LoopCreatorView::stopPreview()
{
    isPlayingPreview_ = false;
    previewBtn_.setButtonText ("Play Preview");
    previewBtn_.setColour (juce::TextButton::buttonColourId, CreatorColors::SurfaceRaised);

    // Trigger Stop
    processor_.transportStop();
}

void LoopCreatorView::compileAndExport()
{
    if (samples_.empty())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "Export Failed", 
            "Please load at least one sample to export.", "OK", this);
        return;
    }

    if (isPlayingPreview_)
        stopPreview();

    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Export .LOOP file",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.loop",
        true);

    fileChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;

            // Ensure correct extension
            if (file.getFileExtension().toLowerCase() != ".loop")
                file = file.withFileExtension(".loop");

            if (compileTo(file))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon, "Export Success", 
                    "Successfully compiled and saved loop project to: " + file.getFileName(), "OK", this);
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Export Failed", 
                    "An error occurred while compiling the loop binary.", "OK", this);
            }
        });
}

} // namespace LoopFormat
