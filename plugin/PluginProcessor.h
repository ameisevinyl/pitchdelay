#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

#include "DelayLine.h"

class PitchDelayProcessor final : public juce::AudioProcessor
{
public:
    PitchDelayProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    // NOTE: reset() is deliberately NOT overridden: transport start/stop must not clear the buffer.
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "PitchDelay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Delay currently in effect, in sample frames (for the editor readout and the tail length).
    int getCurrentDelayFrames() const noexcept { return currentDelayFrames.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void updateDelay();

    pitchdelay::DelayLine delayLine;
    double sampleRate_ = 44100.0;
    std::atomic<int> currentDelayFrames { 0 };

    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterChoice* unitParam = nullptr;
    juce::AudioParameterFloat* customMsParam = nullptr;
    juce::AudioParameterInt* customSamplesParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchDelayProcessor)
};
