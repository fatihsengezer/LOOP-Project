/**
 * @file    LoopPlayerEditor.cpp
 * @brief   Phase 3 custom GUI — LoopPlayerEditor implementation.
 *
 * =============================================================================
 * LAYOUT (default 900 × 560, resizable)
 * =============================================================================
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  HEADER  (48 px)  [LOOP logo]  [filename label]       [Browse…]    │
 *  ├─────────────────────────────────────────────────────────────────────┤
 *  │                                                                     │
 *  │  BEAT GRID  (flex height, min 200 px)                               │
 *  │  ┌──────┬───────────────────────────────────────────────────────┐   │
 *  │  │ S0   │  ████ event ████        ████ event ████              │   │
 *  │  │ S1   │      ████ event ████                                 │   │
 *  │  │ ...  │  <playhead line>                                     │   │
 *  │  └──────┴───────────────────────────────────────────────────────┘   │
 *  │                                                                     │
 *  ├─────────────────────────────────────────────────────────────────────┤
 *  │  BOTTOM (120 px)                                                    │
 *  │  [▶ Pause ■]  [BPM knob]  [LOOP toggle]   [ voice meter dots ]    │
 *  ├─────────────────────────────────────────────────────────────────────┤
 *  │  STATUS (22 px)  [file info / drag hint]                            │
 *  └─────────────────────────────────────────────────────────────────────┘
 *
 * =============================================================================
 */

#include "../include/LoopPlayerEditor.h"
#include "../include/FormatSpec.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <cmath>

namespace LoopFormat
{

// ─────────────────────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int kDefaultW     = 900;
static constexpr int kDefaultH     = 560;
static constexpr int kMinW         = 700;
static constexpr int kMinH         = 420;
static constexpr int kHeaderH      = 48;
static constexpr int kBottomH      = 120;
static constexpr int kStatusH      = 22;
static constexpr int kTimerHz      = 30;


// ═════════════════════════════════════════════════════════════════════════════
//  LoopLookAndFeel
// ═════════════════════════════════════════════════════════════════════════════

LoopLookAndFeel::LoopLookAndFeel()
{
    // Use JUCE built-in Typeface (Inter-like via system)
    primaryFont_ = juce::Font(13.f);

    // Global colour overrides
    setColour(juce::TextButton::buttonColourId,
              EditorColors::SurfaceRaised);
    setColour(juce::TextButton::buttonOnColourId,
              EditorColors::Accent.withAlpha(0.25f));
    setColour(juce::TextButton::textColourOffId,
              EditorColors::TextPrimary);
    setColour(juce::TextButton::textColourOnId,
              EditorColors::Accent);

    setColour(juce::Slider::rotarySliderFillColourId,
              EditorColors::Accent);
    setColour(juce::Slider::rotarySliderOutlineColourId,
              EditorColors::Border);
    setColour(juce::Slider::thumbColourId,
              EditorColors::Accent);
    setColour(juce::Slider::textBoxTextColourId,
              EditorColors::TextPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId,
              EditorColors::Surface);
    setColour(juce::Slider::textBoxOutlineColourId,
              juce::Colours::transparentBlack);

    setColour(juce::Label::textColourId,
              EditorColors::TextPrimary);

    setColour(juce::ToggleButton::textColourId,
              EditorColors::TextSecondary);
    setColour(juce::ToggleButton::tickColourId,
              EditorColors::Accent);
    setColour(juce::ToggleButton::tickDisabledColourId,
              EditorColors::Border);
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopLookAndFeel::drawRotarySlider(juce::Graphics& g,
                                        int x, int y, int width, int height,
                                        float sliderPos,
                                        float rotaryStartAngle,
                                        float rotaryEndAngle,
                                        juce::Slider& /*slider*/)
{
    const float cx   = static_cast<float>(x) + static_cast<float>(width)  * 0.5f;
    const float cy   = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
    const float r    = std::min(static_cast<float>(width), static_cast<float>(height)) * 0.5f - 6.f;

    // ── Outer track ──────────────────────────────────────────────────────────
    juce::Path track;
    track.addArc(cx - r, cy - r, r * 2.f, r * 2.f,
                 rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(EditorColors::Border);
    g.strokePath(track, juce::PathStrokeType(3.f,
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // ── Value arc ────────────────────────────────────────────────────────────
    const float valueAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    juce::Path valueArc;
    valueArc.addArc(cx - r, cy - r, r * 2.f, r * 2.f,
                    rotaryStartAngle, valueAngle, true);
    g.setColour(EditorColors::Accent);
    g.strokePath(valueArc, juce::PathStrokeType(3.f,
        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // ── Glow behind knob ─────────────────────────────────────────────────────
    const float glowR = r * 0.65f;
    juce::ColourGradient glow(
        EditorColors::Accent.withAlpha(0.18f), cx, cy,
        juce::Colours::transparentBlack, cx, cy + glowR, true);
    g.setGradientFill(glow);
    g.fillEllipse(cx - glowR, cy - glowR, glowR * 2.f, glowR * 2.f);

    // ── Knob body ─────────────────────────────────────────────────────────────
    const float kR = r * 0.55f;
    juce::ColourGradient body(
        EditorColors::SurfaceRaised.brighter(0.15f), cx, cy - kR * 0.5f,
        EditorColors::Surface,                        cx, cy + kR,
        false);
    g.setGradientFill(body);
    g.fillEllipse(cx - kR, cy - kR, kR * 2.f, kR * 2.f);
    g.setColour(EditorColors::Border);
    g.drawEllipse(cx - kR, cy - kR, kR * 2.f, kR * 2.f, 1.5f);

    // ── Pointer line ──────────────────────────────────────────────────────────
    const float px = cx + (kR * 0.7f) * std::sin(valueAngle);
    const float py = cy - (kR * 0.7f) * std::cos(valueAngle);
    g.setColour(EditorColors::Accent);
    g.drawLine(cx, cy, px, py, 2.5f);
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopLookAndFeel::drawButtonBackground(juce::Graphics& g,
                                            juce::Button& button,
                                            const juce::Colour& /*bg*/,
                                            bool isHighlighted,
                                            bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.f);
    const float corner = 6.f;

    juce::Colour base = EditorColors::SurfaceRaised;
    if (isDown)        base = EditorColors::Accent.withAlpha(0.35f);
    else if (isHighlighted) base = EditorColors::SurfaceRaised.brighter(0.25f);

    g.setColour(base);
    g.fillRoundedRectangle(bounds, corner);

    const juce::Colour borderColor =
        isDown       ? EditorColors::Accent :
        isHighlighted? EditorColors::Accent.withAlpha(0.6f) :
                       EditorColors::Border;
    g.setColour(borderColor);
    g.drawRoundedRectangle(bounds, corner, 1.f);
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopLookAndFeel::drawToggleButton(juce::Graphics& g,
                                        juce::ToggleButton& button,
                                        bool isHighlighted,
                                        bool isDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.f);
    const float corner = 6.f;

    const bool on = button.getToggleState();

    juce::Colour bg = on ? EditorColors::Accent.withAlpha(0.25f)
                         : EditorColors::SurfaceRaised;
    if (isDown || isHighlighted) bg = bg.brighter(0.1f);

    g.setColour(bg);
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(on ? EditorColors::Accent : EditorColors::Border);
    g.drawRoundedRectangle(bounds, corner, 1.f);

    g.setColour(on ? EditorColors::Accent : EditorColors::TextSecondary);
    g.setFont(juce::Font(12.f, juce::Font::bold));
    g.drawText(button.getButtonText(), bounds, juce::Justification::centred, false);
}

// ─────────────────────────────────────────────────────────────────────────────

juce::Font LoopLookAndFeel::getLabelFont(juce::Label& /*label*/)
{
    return primaryFont_;
}

juce::Font LoopLookAndFeel::getTextButtonFont(juce::TextButton& /*btn*/, int buttonHeight)
{
    return juce::Font(std::min(static_cast<float>(buttonHeight) * 0.5f, 14.f));
}


// ═════════════════════════════════════════════════════════════════════════════
//  BeatGridComponent
// ═════════════════════════════════════════════════════════════════════════════

BeatGridComponent::BeatGridComponent()
{
    setOpaque(true);
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::setEvents(std::vector<CachedEvent> events,
                                   uint32_t totalDurationTicks,
                                   uint32_t tpqn)
{
    events_              = std::move(events);
    totalDurationTicks_  = totalDurationTicks;
    tpqn_                = (tpqn > 0) ? tpqn : 960u;
    mutedSampleIds_.clear();

    // Build ordered row list (unique sampleIds in order of first appearance)
    orderedRows_.clear();
    for (const auto& e : events_)
    {
        if (std::find(orderedRows_.begin(), orderedRows_.end(), e.sampleId)
            == orderedRows_.end())
            orderedRows_.push_back(e.sampleId);
    }

    hoveredIdx_ = -1;
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::clearEvents()
{
    events_.clear();
    orderedRows_.clear();
    mutedSampleIds_.clear();
    totalDurationTicks_ = 0;
    playheadTick_       = 0;
    hoveredIdx_         = -1;
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::setPlayheadTick(uint32_t tick)
{
    if (tick != playheadTick_)
    {
        playheadTick_ = tick;
        repaint();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

bool BeatGridComponent::toggleMuteSampleId(uint16_t sampleId)
{
    if (mutedSampleIds_.count(sampleId))
    {
        mutedSampleIds_.erase(sampleId);
        repaint();
        return false;
    }
    else
    {
        mutedSampleIds_.insert(sampleId);
        repaint();
        return true;
    }
}

bool BeatGridComponent::isSampleMuted(uint16_t sampleId) const noexcept
{
    return mutedSampleIds_.count(sampleId) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Layout helpers
// ─────────────────────────────────────────────────────────────────────────────

float BeatGridComponent::tickToX(uint32_t tick) const noexcept
{
    if (totalDurationTicks_ == 0) return kLabelW;
    const float available = static_cast<float>(getWidth()) - kLabelW;
    return kLabelW + (static_cast<float>(tick) /
                      static_cast<float>(totalDurationTicks_)) * available;
}

float BeatGridComponent::sampleIdToY(uint16_t id) const noexcept
{
    const auto it = std::find(orderedRows_.begin(), orderedRows_.end(), id);
    if (it == orderedRows_.end()) return kHeaderH;
    const float rowIdx = static_cast<float>(std::distance(orderedRows_.begin(), it));
    return kHeaderH + rowIdx * kRowH;
}

juce::Rectangle<float> BeatGridComponent::eventRect(const CachedEvent& e) const noexcept
{
    const float x1 = tickToX(e.tickStart);
    const float x2 = tickToX(e.tickStart + e.durationTicks);
    const float y  = sampleIdToY(e.sampleId);
    const float w  = std::max(x2 - x1, 4.f);
    return { x1, y + 2.f, w, kRowH - 4.f };
}

int BeatGridComponent::hoveredEventIndex(juce::Point<float> pos) const noexcept
{
    // Iterate in reverse so topmost drawn event wins
    for (int i = static_cast<int>(events_.size()) - 1; i >= 0; --i)
    {
        if (eventRect(events_[static_cast<size_t>(i)]).contains(pos))
            return i;
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Paint
// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::paint(juce::Graphics& g)
{
    drawBackground(g);

    if (events_.empty() || totalDurationTicks_ == 0)
    {
        drawEmptyState(g);
        return;
    }

    drawBeatLines(g);
    drawRowLabels(g);
    drawEvents(g);
    drawPlayhead(g);
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::drawBackground(juce::Graphics& g) const
{
    g.fillAll(EditorColors::Background);

    // Header strip
    g.setColour(EditorColors::Surface);
    g.fillRect(0.f, 0.f, static_cast<float>(getWidth()), kHeaderH);

    // Row backgrounds
    for (size_t r = 0; r < orderedRows_.size(); ++r)
    {
        const float y = kHeaderH + static_cast<float>(r) * kRowH;
        g.setColour((r % 2 == 0) ? EditorColors::Background
                                  : EditorColors::Surface.withAlpha(0.5f));
        g.fillRect(0.f, y, static_cast<float>(getWidth()), kRowH);
    }

    // Left label column
    g.setColour(EditorColors::Surface);
    g.fillRect(0.f, 0.f, kLabelW, static_cast<float>(getHeight()));

    // Separator line
    g.setColour(EditorColors::Border);
    g.drawLine(kLabelW, 0.f, kLabelW, static_cast<float>(getHeight()), 1.f);
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::drawBeatLines(juce::Graphics& g) const
{
    if (totalDurationTicks_ == 0 || tpqn_ == 0) return;

    const float h = static_cast<float>(getHeight());

    // How many beats fit?
    const uint32_t beatsTotal = (totalDurationTicks_ + tpqn_ - 1) / tpqn_;
    const uint32_t beatsPerBar = 4; // assume 4/4

    for (uint32_t beat = 0; beat <= beatsTotal; ++beat)
    {
        const float x = tickToX(beat * tpqn_);
        const bool isBarLine = (beat % beatsPerBar == 0);

        if (isBarLine)
        {
            g.setColour(juce::Colour(0xFF2D333B));
            g.drawLine(x, kHeaderH, x, h, 1.5f);

            // Bar number
            g.setColour(EditorColors::TextSecondary);
            g.setFont(juce::Font(10.f));
            g.drawText(juce::String(beat / beatsPerBar + 1),
                       static_cast<int>(x) + 3, 0,
                       40, static_cast<int>(kHeaderH),
                       juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour(juce::Colour(0xFF21262D));
            g.drawLine(x, kHeaderH, x, h, 0.75f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::drawRowLabels(juce::Graphics& g) const
{
    g.setFont(juce::Font(11.f));

    for (size_t r = 0; r < orderedRows_.size(); ++r)
    {
        const uint16_t id = orderedRows_[r];
        const float y = kHeaderH + static_cast<float>(r) * kRowH;
        const bool muted = mutedSampleIds_.count(id) > 0;

        // Color swatch
        const juce::Colour col = sampleColor(id).withAlpha(muted ? 0.35f : 1.f);
        g.setColour(col);
        g.fillRoundedRectangle(6.f, y + 9.f, 8.f, 8.f, 2.f);

        // Label
        g.setColour(muted ? EditorColors::TextSecondary
                           : EditorColors::TextPrimary);
        g.drawText("S" + juce::String(id),
                   18, static_cast<int>(y),
                   static_cast<int>(kLabelW) - 20, static_cast<int>(kRowH),
                   juce::Justification::centredLeft, false);

        if (muted)
        {
            g.setColour(EditorColors::TextSecondary.withAlpha(0.5f));
            g.setFont(juce::Font(9.f));
            g.drawText("MUTED",
                       18, static_cast<int>(y) + static_cast<int>(kRowH / 2),
                       static_cast<int>(kLabelW) - 20, static_cast<int>(kRowH / 2),
                       juce::Justification::centredLeft, false);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::drawEvents(juce::Graphics& g) const
{
    for (int i = 0; i < static_cast<int>(events_.size()); ++i)
    {
        const auto& e    = events_[static_cast<size_t>(i)];
        const auto  rect = eventRect(e);
        const bool  muted    = mutedSampleIds_.count(e.sampleId) > 0;
        const bool  hovered  = (i == hoveredIdx_);

        const float alpha = muted ? 0.2f : 1.f;
        juce::Colour col  = sampleColor(e.sampleId).withAlpha(alpha);

        // Fill
        juce::ColourGradient grad(
            col.brighter(hovered ? 0.5f : 0.2f), rect.getX(), rect.getY(),
            col.darker(0.2f),                     rect.getX(), rect.getBottom(),
            false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(rect, 3.f);

        // Border
        g.setColour(col.brighter(0.4f).withAlpha(muted ? 0.3f : 0.85f));
        g.drawRoundedRectangle(rect, 3.f, 1.f);

        // Velocity bar (thin strip at bottom)
        if (!muted)
        {
            const float velFrac = static_cast<float>(e.velocity) / 127.f;
            const float vw      = rect.getWidth() * velFrac;
            g.setColour(juce::Colours::white.withAlpha(0.15f));
            g.fillRect(rect.getX(), rect.getBottom() - 3.f, vw, 2.f);
        }

        // One-shot indicator
        if (e.isOneShot && rect.getWidth() > 14.f)
        {
            g.setColour(juce::Colours::white.withAlpha(muted ? 0.15f : 0.55f));
            g.setFont(juce::Font(8.f));
            g.drawText("1x", rect.reduced(2.f), juce::Justification::topRight, false);
        }

        // Hover tooltip hint
        if (hovered)
        {
            g.setColour(EditorColors::Accent.withAlpha(0.25f));
            g.drawRoundedRectangle(rect.expanded(1.f), 4.f, 1.5f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::drawPlayhead(juce::Graphics& g) const
{
    if (totalDurationTicks_ == 0) return;

    const float x = tickToX(playheadTick_);
    const float h = static_cast<float>(getHeight());

    // Glow
    juce::ColourGradient glow(
        EditorColors::Accent.withAlpha(0.18f), x, 0.f,
        juce::Colours::transparentBlack,       x + 12.f, 0.f,
        false);
    g.setGradientFill(glow);
    g.fillRect(x - 8.f, kHeaderH, 16.f, h - kHeaderH);

    // Line
    g.setColour(EditorColors::Accent);
    g.drawLine(x, kHeaderH, x, h, 2.f);

    // Triangle head
    juce::Path head;
    head.addTriangle(x - 5.f, kHeaderH, x + 5.f, kHeaderH, x, kHeaderH + 8.f);
    g.setColour(EditorColors::Accent);
    g.fillPath(head);
}

// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::drawEmptyState(juce::Graphics& g) const
{
    const auto b = getLocalBounds().toFloat();
    g.setColour(EditorColors::TextSecondary.withAlpha(0.4f));
    g.setFont(juce::Font(14.f));
    g.drawText("Drop a .loop file here or click Browse\xe2\x80\xa6",
               b, juce::Justification::centred, false);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Mouse
// ─────────────────────────────────────────────────────────────────────────────

void BeatGridComponent::mouseDown(const juce::MouseEvent& e)
{
    if (events_.empty()) return;

    const int idx = hoveredEventIndex(e.position);
    if (idx < 0) return;

    const auto& evt = events_[static_cast<size_t>(idx)];

    if (e.mods.isRightButtonDown())
    {
        // Right-click → toggle mute
        const bool nowMuted = toggleMuteSampleId(evt.sampleId);
        if (onMuteToggled)
            onMuteToggled(evt.sampleId, nowMuted);
    }
    else
    {
        // Left-click → seek
        if (onSeekRequested)
            onSeekRequested(evt.tickStart);
    }
}

void BeatGridComponent::mouseMove(const juce::MouseEvent& e)
{
    const int newHover = hoveredEventIndex(e.position);
    if (newHover != hoveredIdx_)
    {
        hoveredIdx_ = newHover;
        setMouseCursor(newHover >= 0 ? juce::MouseCursor::PointingHandCursor
                                     : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void BeatGridComponent::mouseExit(const juce::MouseEvent&)
{
    hoveredIdx_ = -1;
    repaint();
}


// ═════════════════════════════════════════════════════════════════════════════
//  VoiceMeterComponent
// ═════════════════════════════════════════════════════════════════════════════

VoiceMeterComponent::VoiceMeterComponent()
{
    setOpaque(false);
}

// ─────────────────────────────────────────────────────────────────────────────

void VoiceMeterComponent::setActiveVoiceCount(int count) noexcept
{
    if (count != activeCount_)
    {
        activeCount_ = count;
        repaint();
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void VoiceMeterComponent::paint(juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();

    // Title
    g.setColour(EditorColors::TextSecondary);
    g.setFont(juce::Font(10.f));
    g.drawText("VOICES", b.withHeight(14.f), juce::Justification::centred, false);

    // Dots area
    const float dotAreaY = 14.f;
    const float dotAreaH = b.getHeight() - dotAreaY - 14.f;
    const float dotAreaW = b.getWidth();

    const int cols = 8;
    const int rows = 2;
    const float dotW  = dotAreaW / static_cast<float>(cols);
    const float dotH  = dotAreaH / static_cast<float>(rows);
    const float dotR  = std::min(dotW, dotH) * 0.35f;

    for (int i = 0; i < kTotalDots; ++i)
    {
        const int col = i % cols;
        const int row = i / cols;
        const float cx = (static_cast<float>(col) + 0.5f) * dotW;
        const float cy = dotAreaY + (static_cast<float>(row) + 0.5f) * dotH;
        const bool  lit = (i < activeCount_);

        if (lit)
        {
            // Glow
            juce::ColourGradient glow(
                EditorColors::Accent.withAlpha(0.4f), cx, cy,
                juce::Colours::transparentBlack, cx + dotR * 2.f, cy,
                true);
            g.setGradientFill(glow);
            g.fillEllipse(cx - dotR * 2.f, cy - dotR * 2.f, dotR * 4.f, dotR * 4.f);

            g.setColour(EditorColors::Accent);
        }
        else
        {
            g.setColour(EditorColors::Border);
        }
        g.fillEllipse(cx - dotR, cy - dotR, dotR * 2.f, dotR * 2.f);
    }

    // Count label
    g.setColour(activeCount_ > 0 ? EditorColors::Accent : EditorColors::TextSecondary);
    g.setFont(juce::Font(11.f));
    g.drawText(juce::String(activeCount_) + " / 64",
               b.withTrimmedTop(b.getHeight() - 14.f),
               juce::Justification::centred, false);
}


// ═════════════════════════════════════════════════════════════════════════════
//  LoopPlayerEditor
// ═════════════════════════════════════════════════════════════════════════════

LoopPlayerEditor::LoopPlayerEditor(LoopPlayerProcessor& processor)
    : AudioProcessorEditor(processor)
    , processor_(processor)
{
    logDebug("LoopPlayerEditor ctor start");
    setLookAndFeel(&laf_);
    setResizable(true, true);
    setResizeLimits(kMinW, kMinH, 1920, 1080);

    buildComponents();
    buildTransportButtons();
    buildBpmKnob();
    connectApvts();

    // Mode toggles
    playerModeBtn_.setClickingTogglesState (true);
    creatorModeBtn_.setClickingTogglesState (true);
    playerModeBtn_.setToggleState (true, juce::dontSendNotification);
    creatorModeBtn_.setToggleState (false, juce::dontSendNotification);
    playerModeBtn_.onClick = [this] { setViewMode (true); };
    creatorModeBtn_.onClick = [this] { setViewMode (false); };
    addAndMakeVisible (playerModeBtn_);
    addAndMakeVisible (creatorModeBtn_);

    logDebug("LoopPlayerEditor ctor: creating LoopCreatorView");
    creatorView_ = std::make_unique<LoopCreatorView> (processor_, [this](const juce::File& tempFile) {
        tryLoadFile (tempFile);
    });
    addAndMakeVisible (creatorView_.get());

    // If a file is already loaded (e.g. session restore) refresh the grid
    if (processor_.hasFileLoaded())
    {
        logDebug("LoopPlayerEditor ctor: session restore file loaded, refreshing sequence data");
        refreshSequenceData();
    }

    updateFileLabel();
    startTimerHz(kTimerHz);

    setViewMode (true);

    logDebug("LoopPlayerEditor ctor: setting size");
    setSize(kDefaultW, kDefaultH);
    logDebug("LoopPlayerEditor ctor done");
}

// ─────────────────────────────────────────────────────────────────────────────

LoopPlayerEditor::~LoopPlayerEditor()
{
    stopTimer();

    // Detach APVTS before destroying components
    bpmAttachment_.reset();
    loopAttachment_.reset();

    setLookAndFeel(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  buildComponents
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::buildComponents()
{
    // Logo label
    logoLabel_.setText("LOOP", juce::dontSendNotification);
    logoLabel_.setFont(
        juce::Font(22.f, juce::Font::bold));
    logoLabel_.setColour(juce::Label::textColourId, EditorColors::Accent);
    logoLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(logoLabel_);

    // File label
    fileLabel_.setFont(juce::Font(12.f));
    fileLabel_.setColour(juce::Label::textColourId, EditorColors::TextSecondary);
    fileLabel_.setJustificationType(juce::Justification::centred);
    fileLabel_.setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(fileLabel_);

    // Browse button
    browseButton_.setButtonText("Browse\xe2\x80\xa6");
    browseButton_.onClick = [this] { launchFileBrowser(); };
    addAndMakeVisible(browseButton_);

    // Beat grid
    beatGrid_.onSeekRequested = [this](uint32_t tick)
    {
        processor_.transportSeek(tick);
    };
    beatGrid_.onMuteToggled = [this](uint16_t /*id*/, bool /*muted*/)
    {
        syncMutesToSequencer();
    };
    addAndMakeVisible(beatGrid_);

    // Voice meter
    addAndMakeVisible(voiceMeter_);

    // BPM title
    bpmTitleLabel_.setText("BPM", juce::dontSendNotification);
    bpmTitleLabel_.setFont(juce::Font(10.f));
    bpmTitleLabel_.setColour(juce::Label::textColourId, EditorColors::TextSecondary);
    bpmTitleLabel_.setJustificationType(juce::Justification::centredTop);
    addAndMakeVisible(bpmTitleLabel_);

    // Status label
    statusLabel_.setFont(juce::Font(11.f));
    statusLabel_.setColour(juce::Label::textColourId, EditorColors::TextSecondary);
    statusLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(statusLabel_);
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::buildTransportButtons()
{
    // ▶ Play
    playButton_.setButtonText("\xe2\x96\xb6  Play");
    playButton_.onClick = [this]
    {
        processor_.transportPlay();
    };
    addAndMakeVisible(playButton_);

    // ⏸ Pause
    pauseButton_.setButtonText("\xe2\x8f\xb8  Pause");
    pauseButton_.onClick = [this]
    {
        processor_.transportPause();
    };
    addAndMakeVisible(pauseButton_);

    // ■ Stop
    stopButton_.setButtonText("\xe2\x96\xa0  Stop");
    stopButton_.onClick = [this]
    {
        processor_.transportStop();
    };
    addAndMakeVisible(stopButton_);

    // Loop toggle (no text — handled by custom drawToggleButton)
    loopButton_.setButtonText("LOOP");
    addAndMakeVisible(loopButton_);
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::buildBpmKnob()
{
    bpmKnob_.setSliderStyle(juce::Slider::RotaryVerticalDrag);
    bpmKnob_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 18);
    bpmKnob_.setRange(20.0, 400.0, 0.01);
    bpmKnob_.setSkewFactorFromMidPoint(120.0);
    bpmKnob_.setDoubleClickReturnValue(true, 120.0);
    bpmKnob_.setScrollWheelEnabled(true);
    addAndMakeVisible(bpmKnob_);
    addAndMakeVisible(bpmTitleLabel_);
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::connectApvts()
{
    bpmAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor_.apvts, LoopPlayerProcessor::kParamBPM, bpmKnob_);

    loopAttachment_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        processor_.apvts, LoopPlayerProcessor::kParamLooping, loopButton_);
}

// ─────────────────────────────────────────────────────────────────────────────
//  paint / resized
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::paint(juce::Graphics& g)
{
    // Background
    g.fillAll(EditorColors::Background);

    // Header bar background
    g.setColour(EditorColors::Surface);
    g.fillRect(0, 0, getWidth(), kHeaderH);

    // Separator under header
    g.setColour(EditorColors::Border);
    g.drawLine(0.f, static_cast<float>(kHeaderH),
               static_cast<float>(getWidth()), static_cast<float>(kHeaderH), 1.f);

    if (isPlayerMode_)
    {
        // Bottom panel background
        const int bottomY = getHeight() - kBottomH - kStatusH;
        g.setColour(EditorColors::Surface);
        g.fillRect(0, bottomY, getWidth(), kBottomH);
        g.setColour(EditorColors::Border);
        g.drawLine(0.f, static_cast<float>(bottomY),
                   static_cast<float>(getWidth()), static_cast<float>(bottomY), 1.f);

        // Status bar background
        g.setColour(juce::Colour(0xFF0A0E13));
        g.fillRect(0, getHeight() - kStatusH, getWidth(), kStatusH);
        g.setColour(EditorColors::Border);
        g.drawLine(0.f, static_cast<float>(getHeight() - kStatusH),
                   static_cast<float>(getWidth()), static_cast<float>(getHeight() - kStatusH), 1.f);

        // Vertical divider between transport and voice meter
        const int meterW = 160;
        const int divX   = getWidth() - meterW;
        g.setColour(EditorColors::Border);
        g.drawLine(static_cast<float>(divX), static_cast<float>(bottomY),
                   static_cast<float>(divX), static_cast<float>(getHeight() - kStatusH), 1.f);
    }

    // Drag-over overlay
    if (isDraggingFile_)
    {
        g.setColour(EditorColors::Accent.withAlpha(0.12f));
        g.fillAll();

        g.setColour(EditorColors::Accent);
        const float dash[] = { 8.f, 4.f };
        juce::Path border;
        border.addRectangle(getLocalBounds().toFloat().reduced(8.f));
        g.strokePath(border,
            juce::PathStrokeType(2.f),
            juce::AffineTransform());

        g.setFont(juce::Font(18.f));
        g.setColour(EditorColors::Accent);
        g.drawText("Drop .loop file to load",
                   getLocalBounds(), juce::Justification::centred, false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::resized()
{
    const int w = getWidth();
    const int h = getHeight();

    // ── Header row ──────────────────────────────────────────────────────────
    const int headerPad = 10;
    logoLabel_.setBounds(headerPad, 0, 70, kHeaderH);
    browseButton_.setBounds(w - 100 - headerPad, (kHeaderH - 26) / 2, 100, 26);

    // Segmented button toggles for mode selection
    const int toggleW = 80;
    const int toggleX = w - 100 - headerPad - toggleW * 2 - 20;
    playerModeBtn_.setBounds(toggleX, (kHeaderH - 26) / 2, toggleW, 26);
    creatorModeBtn_.setBounds(toggleX + toggleW, (kHeaderH - 26) / 2, toggleW, 26);

    fileLabel_.setBounds(80, 0, toggleX - 90, kHeaderH);

    if (creatorView_ != nullptr)
        creatorView_->setBounds(0, kHeaderH + 1, w, h - kHeaderH - 1);

    // ── Beat grid ───────────────────────────────────────────────────────────
    const int gridTop  = kHeaderH + 1;
    const int gridBot  = h - kBottomH - kStatusH;
    beatGrid_.setBounds(0, gridTop, w, gridBot - gridTop);

    // ── Bottom panel ────────────────────────────────────────────────────────
    const int bottomY  = gridBot;
    const int meterW   = 160;

    // Transport buttons (left side of bottom panel)
    const int btnH  = 32;
    const int btnW  = 88;
    const int btnY  = bottomY + (kBottomH - btnH) / 2;
    const int btnGap = 6;

    playButton_.setBounds (12,                    btnY, btnW, btnH);
    pauseButton_.setBounds(12 + btnW + btnGap,    btnY, btnW, btnH);
    stopButton_.setBounds (12 + (btnW + btnGap)*2, btnY, btnW, btnH);

    loopButton_.setBounds(12 + (btnW + btnGap)*3 + 10, btnY + (btnH - 26) / 2, 64, 26);

    // BPM rotary knob (centred in remaining transport space)
    const int knobSize = 80;
    const int knobX    = 12 + (btnW + btnGap) * 3 + 10 + 74 + 20;
    const int knobY    = bottomY + (kBottomH - knobSize) / 2;
    bpmKnob_.setBounds(knobX, knobY, knobSize, knobSize);
    bpmTitleLabel_.setBounds(knobX, bottomY + 4, knobSize, 14);

    // Voice meter (right side of bottom panel)
    voiceMeter_.setBounds(w - meterW, bottomY + 4, meterW, kBottomH - 8);

    // ── Status bar ──────────────────────────────────────────────────────────
    statusLabel_.setBounds(8, h - kStatusH, w - 16, kStatusH);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Timer callback (30 Hz)
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::timerCallback()
{
    if (! isPlayerMode_)
        return;

    const bool hasFile = processor_.hasFileLoaded();

    // Detect file load / unload
    if (hasFile != hadFileLastTick_)
    {
        hadFileLastTick_ = hasFile;
        if (hasFile)
            refreshSequenceData();
        else
            beatGrid_.clearEvents();

        updateFileLabel();
    }

    // Poll playhead tick
    if (hasFile)
    {
        if (auto* seq = processor_.getSequencer())
        {
            const uint32_t tick = seq->getPlayheadTick();
            if (tick != lastPlayheadTick_)
            {
                lastPlayheadTick_ = tick;
                beatGrid_.setPlayheadTick(tick);
            }

            const int voices = seq->getActiveVoiceCount();
            if (voices != lastVoiceCount_)
            {
                lastVoiceCount_ = voices;
                voiceMeter_.setActiveVoiceCount(voices);
            }

            // Update status strip with timing info
            const float bpm  = seq->getBPM();
            const auto  state = seq->getTransportState();
            const juce::String stateStr =
                (state == TransportState::Playing) ? "Playing" :
                (state == TransportState::Paused)  ? "Paused"  : "Stopped";

            const uint32_t seqDur = seq->getSequenceDurationTicks();
            const float beatPos = (seqDur > 0)
                ? static_cast<float>(tick) / static_cast<float>(960)
                : 0.f;

            statusLabel_.setText(
                stateStr + "  |  " +
                juce::String(beatPos, 2) + " beats  |  " +
                juce::String(bpm, 1) + " BPM  |  " +
                juce::String(voices) + " voices",
                juce::dontSendNotification);
        }
    }
    else
    {
        statusLabel_.setText(
            "No file loaded — drag a .loop file here or click Browse\xe2\x80\xa6",
            juce::dontSendNotification);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  File drag & drop
// ─────────────────────────────────────────────────────────────────────────────

bool LoopPlayerEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".loop"))
            return true;
    return false;
}

void LoopPlayerEditor::fileDragEnter(const juce::StringArray&, int, int)
{
    isDraggingFile_ = true;
    repaint();
}

void LoopPlayerEditor::fileDragExit(const juce::StringArray&)
{
    isDraggingFile_ = false;
    repaint();
}

void LoopPlayerEditor::filesDropped(const juce::StringArray& files, int, int)
{
    isDraggingFile_ = false;
    repaint();

    for (const auto& f : files)
    {
        if (f.endsWithIgnoreCase(".loop"))
        {
            tryLoadFile(juce::File(f));
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  File operations
// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::tryLoadFile(const juce::File& file)
{
    fileLabel_.setText("Loading " + file.getFileName() + "\xe2\x80\xa6",
                       juce::dontSendNotification);

    const auto err = processor_.loadFile(file);
    if (err != LoopPlayerProcessor::LoadError::None)
    {
        const juce::String msg =
            err == LoopPlayerProcessor::LoadError::FileNotFound
                ? "File not found: " + file.getFileName()
                : "Failed to load: " + file.getFileName() + " (format error)";

        fileLabel_.setText(msg, juce::dontSendNotification);
        fileLabel_.setColour(juce::Label::textColourId, EditorColors::Danger);

        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Load Error", msg, "OK", this);
        return;
    }

    fileLabel_.setColour(juce::Label::textColourId, EditorColors::TextSecondary);
    refreshSequenceData();
    updateFileLabel();
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::launchFileBrowser()
{
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Open .LOOP File",
        processor_.hasFileLoaded()
            ? processor_.getLoadedFile().getParentDirectory()
            : juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.loop",
        true /*useOS*/);

    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc)
        {
            const auto result = fc.getResult();
            if (result.existsAsFile())
                tryLoadFile(result);
        });
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::refreshSequenceData()
{
    const auto* reader = processor_.getReader();
    if (!reader || !reader->isValid())
    {
        beatGrid_.clearEvents();
        return;
    }

    const SequenceHeader seqHdr = reader->getSequenceHeader();
    const FileHeader     fileHdr = reader->getFileHeader();
    const uint32_t       eventCount = reader->getEventCount();

    // Build cached event list
    std::vector<CachedEvent> evts;
    evts.reserve(static_cast<size_t>(eventCount));

    const SequenceEvent* begin = reader->getEventsBegin();
    const SequenceEvent* end   = reader->getEventsEnd();

    for (const SequenceEvent* p = begin; p != end; ++p)
    {
        // Sanity check sample ID
        if (p->sampleID() >= fileHdr.sample_count) continue;

        CachedEvent ce;
        ce.tickStart     = p->tickStart();
        ce.durationTicks = (p->loopDuration() > 0)
                           ? p->loopDuration()
                           : static_cast<uint32_t>(fileHdr.tpqn);
        ce.sampleId      = p->sampleID();
        ce.velocity      = p->velocity();
        ce.isOneShot     = p->isOneShot();
        evts.push_back(ce);
    }

    beatGrid_.setEvents(std::move(evts),
                         seqHdr.total_duration_ticks,
                         static_cast<uint32_t>(fileHdr.tpqn));
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::updateFileLabel()
{
    if (!processor_.hasFileLoaded())
    {
        fileLabel_.setText("No file loaded", juce::dontSendNotification);
        fileLabel_.setColour(juce::Label::textColourId, EditorColors::TextSecondary);
        return;
    }

    const juce::File f = processor_.getLoadedFile();
    const auto* reader = processor_.getReader();

    juce::String info = f.getFileName();
    if (reader && reader->isValid())
    {
        const auto seqHdr  = reader->getSequenceHeader();
        const auto fileHdr = reader->getFileHeader();
        info += "  \xe2\x80\x94  "
              + juce::String(fileHdr.sample_count) + " samples  "
              + juce::String(reader->getEventCount()) + " events  "
              + juce::String(seqHdr.bpm, 1) + " BPM";
    }
    fileLabel_.setText(info, juce::dontSendNotification);
    fileLabel_.setColour(juce::Label::textColourId, EditorColors::TextSecondary);
}

// ─────────────────────────────────────────────────────────────────────────────

void LoopPlayerEditor::syncMutesToSequencer()
{
    auto* seq = processor_.getSequencer();
    if (!seq) return;

    const auto& mutedIds = beatGrid_.getMutedIds();
    uint64_t mask = 0;
    for (uint16_t id : mutedIds)
        if (id < 64) mask |= (1ULL << id);

    seq->setMutedMask(mask);
}

void LoopPlayerEditor::setViewMode (bool isPlayerMode)
{
    isPlayerMode_ = isPlayerMode;

    playerModeBtn_.setToggleState (isPlayerMode, juce::dontSendNotification);
    creatorModeBtn_.setToggleState (!isPlayerMode, juce::dontSendNotification);

    const auto visibility = isPlayerMode;
    beatGrid_.setVisible (visibility);
    voiceMeter_.setVisible (visibility);
    fileLabel_.setVisible (visibility);
    browseButton_.setVisible (visibility);
    playButton_.setVisible (visibility);
    pauseButton_.setVisible (visibility);
    stopButton_.setVisible (visibility);
    loopButton_.setVisible (visibility);
    bpmKnob_.setVisible (visibility);
    bpmTitleLabel_.setVisible (visibility);
    statusLabel_.setVisible (visibility);

    if (creatorView_ != nullptr)
        creatorView_->setVisible (!isPlayerMode);

    if (!isPlayerMode)
    {
        processor_.transportStop();
    }

    resized();
    repaint();
}

} // namespace LoopFormat
