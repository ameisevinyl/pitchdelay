#include "PluginEditor.h"

#include <algorithm>
#include <optional>

#include "DelaySettings.h"
#include "ParameterIds.h"

namespace
{
constexpr long long kHugeNumber = 2000000000LL;

// Digits only (surrounding spaces allowed). Anything else is rejected.
std::optional<long long> parseWholeNumber (const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty() || ! t.containsOnly ("0123456789"))
        return std::nullopt;
    if (t.length() > 12)
        return kHugeNumber;              // far above any maximum; clamped by the processor
    return std::min (t.getLargeIntValue(), kHugeNumber);
}

// A non-negative decimal with '.' or ',' as separator.
std::optional<double> parseDecimal (const juce::String& text)
{
    const auto t = text.trim().replaceCharacter (',', '.');
    if (t.isEmpty() || ! t.containsOnly ("0123456789.") || t.indexOfChar ('.') != t.lastIndexOfChar ('.'))
        return std::nullopt;
    return t.getDoubleValue();
}
}

StepField::StepField()
{
    field.setEditable (true);
    field.setJustificationType (juce::Justification::centredRight);
    field.setColour (juce::Label::outlineColourId, juce::Colours::grey);
    field.onTextChange = [this] { if (onTextEntered) onTextEntered (field.getText()); };

    up.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xb2"));     // ▲
    down.setButtonText (juce::CharPointer_UTF8 ("\xe2\x96\xbc"));   // ▼
    for (auto* b : { &up, &down })
    {
        b->setTriggeredOnMouseDown (true);
        b->setRepeatSpeed (400, 60);
    }
    up.onClick = [this] { if (onStep) onStep (1); };
    down.onClick = [this] { if (onStep) onStep (-1); };

    addAndMakeVisible (field);
    addAndMakeVisible (up);
    addAndMakeVisible (down);
}

void StepField::setDisplayedText (const juce::String& text)
{
    if (! field.isBeingEdited())
        field.setText (text, juce::dontSendNotification);
}

void StepField::resized()
{
    auto area = getLocalBounds();
    down.setBounds (area.removeFromRight (30));
    up.setBounds (area.removeFromRight (30));
    area.removeFromRight (4);
    field.setBounds (area);
}

PitchDelayEditor::PitchDelayEditor (PitchDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    speedBox.addItemList ({ "33 1/3 RPM", "45 RPM" }, 1);
    fractionBox.addItemList ({ "1/1", "1/2", "1/4", "1/8", "1/16" }, 1);

    levelSlider.setSliderStyle (juce::Slider::IncDecButtons);
    levelSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 24);
    levelSlider.setTextValueSuffix (" dB");

    speedAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::mode, speedBox);
    fractionAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::fraction, fractionBox);
    calibrationAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, pitchdelay::ids::calibration, calibrationToggle);
    levelAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, pitchdelay::ids::calibrationLevel, levelSlider);

    samplesField.onTextEntered = [this] (const juce::String& t) { samplesTyped (t); };
    samplesField.onStep = [this] (int direction)
    {
        proc.setDelayFrames (proc.getCurrentDelayFrames() + direction);
        updateFromProcessor();
    };
    millisecondsField.onTextEntered = [this] (const juce::String& t) { millisecondsTyped (t); };
    millisecondsField.onStep = [this] (int direction)
    {
        const double sr = proc.getSampleRate();
        if (sr <= 0.0)
            return;
        const double ms = pitchdelay::millisecondsForFrames (proc.getCurrentDelayFrames(), sr)
                          + static_cast<double> (direction);
        proc.setDelayFrames (pitchdelay::framesForMilliseconds (std::max (0.0, ms), sr));
        updateFromProcessor();
    };

    latency.setText ("Reported latency: 0", juce::dontSendNotification);
    readout.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    status.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    status.setColour (juce::Label::textColourId, juce::Colours::orange);

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &speedLabel, &speedBox, &fractionLabel, &fractionBox, &samplesLabel, &samplesField,
             &millisecondsLabel, &millisecondsField, &calibrationToggle, &levelLabel, &levelSlider,
             &readout, &status, &latency })
        addAndMakeVisible (c);

    setSize (460, 360);
    updateFromProcessor();
    startTimerHz (15);
}

void PitchDelayEditor::paint (juce::Graphics& g)
{
    // Hosts do not paint behind a plugin editor; without this the light label text is unreadable.
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void PitchDelayEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    const auto row = [&area] (juce::Component& label, juce::Component& control)
    {
        auto r = area.removeFromTop (28);
        label.setBounds (r.removeFromLeft (130));
        control.setBounds (r);
        area.removeFromTop (6);
    };
    row (speedLabel, speedBox);
    row (fractionLabel, fractionBox);
    row (samplesLabel, samplesField);
    row (millisecondsLabel, millisecondsField);
    area.removeFromTop (8);
    calibrationToggle.setBounds (area.removeFromTop (28));
    area.removeFromTop (6);
    row (levelLabel, levelSlider);
    area.removeFromTop (8);
    readout.setBounds (area.removeFromTop (28));
    status.setBounds (area.removeFromTop (24));
    latency.setBounds (area.removeFromTop (24));
}

void PitchDelayEditor::samplesTyped (const juce::String& text)
{
    if (const auto value = parseWholeNumber (text))
        proc.setDelayFrames (static_cast<int> (*value));
    updateFromProcessor();   // restores the canonical text, also after rejected input
}

void PitchDelayEditor::millisecondsTyped (const juce::String& text)
{
    const double sr = proc.getSampleRate();
    // 1.0e7 ms is far above the 10 s maximum and keeps the frame conversion away from overflow;
    // the processor clamps to the real maximum.
    if (const auto value = parseDecimal (text); value && sr > 0.0)
        proc.setDelayFrames (pitchdelay::framesForMilliseconds (std::min (*value, 1.0e7), sr));
    updateFromProcessor();
}

void PitchDelayEditor::updateFromProcessor()
{
    const int frames = proc.getCurrentDelayFrames();
    const double sr = proc.getSampleRate();
    const double ms = pitchdelay::millisecondsForFrames (frames, sr);

    samplesField.setDisplayedText (juce::String (frames));
    millisecondsField.setDisplayedText (juce::String (ms, 3));

    readout.setText ("Delay: " + juce::String (frames) + " frames = " + juce::String (ms, 3)
                         + " ms @ " + juce::String (sr / 1000.0, 1) + " kHz",
                     juce::dontSendNotification);

    const bool calibrating = proc.apvts.getRawParameterValue (pitchdelay::ids::calibration)->load() >= 0.5f;
    status.setText (calibrating ? "CALIBRATION: pulse every " + juce::String (frames) + " frames"
                                : juce::String(),
                    juce::dontSendNotification);
}

juce::AudioProcessorEditor* PitchDelayProcessor::createEditor()
{
    return new PitchDelayEditor (*this);
}
