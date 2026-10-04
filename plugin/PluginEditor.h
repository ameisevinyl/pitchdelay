#pragma once

#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

// A number box with ▲/▼ buttons. Typing goes through the label; the buttons repeat while held.
class StepField final : public juce::Component
{
public:
    StepField();

    std::function<void (const juce::String&)> onTextEntered;   // user typed + committed text
    std::function<void (int direction)> onStep;                // +1 (▲) or -1 (▼)

    // Ignored while the user is editing the text, so a refresh never eats typing.
    void setDisplayedText (const juce::String& text);

    juce::Label& getLabel() noexcept { return field; }
    juce::TextButton& getUpButton() noexcept { return up; }
    juce::TextButton& getDownButton() noexcept { return down; }

    void resized() override;

private:
    juce::Label field;
    juce::TextButton up, down;
};

class PitchDelayEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PitchDelayEditor (PitchDelayProcessor&);
    ~PitchDelayEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Refreshes the fields and the readout from the processor (also run by the timer).
    void updateFromProcessor();

    StepField& getSamplesField() noexcept { return samplesField; }
    StepField& getMillisecondsField() noexcept { return millisecondsField; }
    juce::String getReadoutText() const { return readout.getText(); }
    juce::String getStatusText() const { return status.getText(); }

private:
    void timerCallback() override { updateFromProcessor(); }
    void samplesTyped (const juce::String& text);
    void millisecondsTyped (const juce::String& text);

    PitchDelayProcessor& proc;

    juce::Label speedLabel { {}, "Speed" }, fractionLabel { {}, "Fraction" },
                samplesLabel { {}, "Samples" }, millisecondsLabel { {}, "Milliseconds" },
                levelLabel { {}, "Pulse level" }, readout, status, latency;
    juce::ComboBox speedBox, fractionBox;
    StepField samplesField, millisecondsField;
    juce::ToggleButton calibrationToggle { "Calibration" };
    juce::Slider levelSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> speedAtt, fractionAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> calibrationAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAtt;
};
