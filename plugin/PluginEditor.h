#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

class PitchDelayEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit PitchDelayEditor (PitchDelayProcessor&);
    ~PitchDelayEditor() override = default;

    void resized() override;

private:
    void timerCallback() override;

    PitchDelayProcessor& proc;

    juce::Label modeLabel { {}, "Mode" }, unitLabel { {}, "Custom unit" },
                msLabel { {}, "Custom (ms)" }, samplesLabel { {}, "Custom (samples)" },
                readout, latency;
    juce::ComboBox modeBox, unitBox;
    juce::Slider msSlider, samplesSlider;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt, unitAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> msAtt, samplesAtt;
};
