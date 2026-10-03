#include "PluginEditor.h"

#include "ParameterIds.h"

PitchDelayEditor::PitchDelayEditor (PitchDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    modeBox.addItemList ({ "33 1/3 RPM", "45 RPM", "Custom" }, 1);
    unitBox.addItemList ({ "Milliseconds", "Samples" }, 1);
    for (auto* s : { &msSlider, &samplesSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 100, 22);
    }

    modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::mode, modeBox);
    unitAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        proc.apvts, pitchdelay::ids::customUnit, unitBox);
    msAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, pitchdelay::ids::customMs, msSlider);
    samplesAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, pitchdelay::ids::customSamples, samplesSlider);

    latency.setText ("Reported latency: 0", juce::dontSendNotification);
    readout.setFont (juce::FontOptions (16.0f, juce::Font::bold));

    for (juce::Component* c : std::initializer_list<juce::Component*> {
             &modeLabel, &modeBox, &unitLabel, &unitBox, &msLabel, &msSlider,
             &samplesLabel, &samplesSlider, &readout, &latency })
        addAndMakeVisible (c);

    setSize (460, 250);
    startTimerHz (15);
    timerCallback();
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
    row (modeLabel, modeBox);
    row (unitLabel, unitBox);
    row (msLabel, msSlider);
    row (samplesLabel, samplesSlider);
    area.removeFromTop (8);
    readout.setBounds (area.removeFromTop (28));
    latency.setBounds (area.removeFromTop (24));
}

void PitchDelayEditor::timerCallback()
{
    const bool custom = modeBox.getSelectedItemIndex() == 2;
    const bool ms = unitBox.getSelectedItemIndex() == 0;
    unitBox.setEnabled (custom);
    msSlider.setEnabled (custom && ms);
    samplesSlider.setEnabled (custom && ! ms);

    const int frames = proc.getCurrentDelayFrames();
    const double sr = proc.getSampleRate();
    readout.setText ("Delay: " + juce::String (frames) + " frames = "
                         + juce::String (sr > 0.0 ? frames * 1000.0 / sr : 0.0, 3) + " ms @ "
                         + juce::String (sr / 1000.0, 1) + " kHz",
                     juce::dontSendNotification);
}

juce::AudioProcessorEditor* PitchDelayProcessor::createEditor()
{
    return new PitchDelayEditor (*this);
}
