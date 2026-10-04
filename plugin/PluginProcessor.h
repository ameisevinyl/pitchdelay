#pragma once

#include <atomic>
#include <juce_audio_processors/juce_audio_processors.h>

#include "DelayLine.h"
#include "PulseGenerator.h"

class PitchDelayProcessor final : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener
{
public:
    PitchDelayProcessor();
    ~PitchDelayProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    // Clears the delay buffer and the pulse phase so the next start begins from silence.
    void reset() override;
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

    // The delay in sample frames (what the editor fields show). Safe to read from any thread.
    int getCurrentDelayFrames() const noexcept { return delayFrames.load(); }
    // Sets the delay; clamped to [0, max]. Ignored before prepareToPlay (the rate is unknown).
    void setDelayFrames (int frames);

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void applyBaseDelay();
    int baseDelayAt (double sampleRate) const;
    void updateDsp();
    bool hostIsStopped() const;

    pitchdelay::DelayLine delayLine;
    pitchdelay::PulseGenerator pulse;
    bool calibrating = false;   // audio thread only: true while the pulse phase is running

    // Serialises the non-audio threads that read-modify-write the delay state (prepareToPlay,
    // setStateInformation, setDelayFrames, the Speed/Fraction listener). The audio thread never
    // takes it: it reads the delayFrames atomic once per block.
    juce::CriticalSection delayStateLock;

    std::atomic<double> sampleRate_ { 0.0 };
    std::atomic<int> delayFrames { 0 };   // the delay in effect, in frames at sampleRate_ (the audio thread reads this)

    // The rest is guarded by delayStateLock. A delay is either "untouched" (recomputed exactly from
    // Speed and Fraction at every sample rate) or "tuned" (typed or stepped by the user). A tuned delay
    // remembers the frames and the rate it was set at, and every later rate is derived from that anchor
    // instead of from a rounded copy, so its time never drifts over repeated rate changes.
    bool delayTuned = false;
    int anchorFrames = 0;
    double anchorRate = 0.0;               // 0 = adopt the rate of the next prepareToPlay
    double pendingLegacyMs = -1.0;         // first-release Custom ms waiting for a known rate

    juce::AudioParameterChoice* modeParam = nullptr;
    juce::AudioParameterChoice* fractionParam = nullptr;
    juce::AudioParameterBool* calibrationParam = nullptr;
    juce::AudioParameterFloat* calibrationLevelParam = nullptr;
    juce::AudioParameterBool* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchDelayProcessor)
};
