#include "PluginProcessor.h"

#include "DelaySettings.h"
#include "ParameterIds.h"

using namespace pitchdelay;

juce::AudioProcessorValueTreeState::ParameterLayout PitchDelayProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::mode, 1 }, "Mode",
        StringArray { "33 1/3 RPM", "45 RPM", "Custom" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::customUnit, 1 }, "Custom unit",
        StringArray { "Milliseconds", "Samples" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::customMs, 1 }, "Custom (ms)",
        NormalisableRange<float> (0.0f, (float) kMaxCustomMs, 0.001f), 900.0f,
        AudioParameterFloatAttributes().withLabel ("ms").withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterInt> (
        ParameterID { ids::customSamples, 1 }, "Custom (samples)",
        0, kMaxCustomSamples, 39690,
        AudioParameterIntAttributes().withLabel ("samples").withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ids::bypass, 1 }, "Bypass", false));

    return layout;
}

PitchDelayProcessor::PitchDelayProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PitchDelayState", createLayout())
{
    modeParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ids::mode));
    unitParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ids::customUnit));
    customMsParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ids::customMs));
    customSamplesParam = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (ids::customSamples));
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ids::bypass));
    jassert (modeParam && unitParam && customMsParam && customSamplesParam && bypassParam);
}

bool PitchDelayProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void PitchDelayProcessor::prepareToPlay (double sampleRate, int)
{
    sampleRate_ = sampleRate;
    const int channels = std::max ({ 1, getTotalNumInputChannels(), getTotalNumOutputChannels() });
    delayLine.prepare (channels, maxDelayFrames (sampleRate));   // allocates and clears
    updateDelay();
}

void PitchDelayProcessor::updateDelay()
{
    const int frames = computeDelayFrames (static_cast<Mode> (modeParam->getIndex()),
                                           static_cast<CustomUnit> (unitParam->getIndex()),
                                           (double) customMsParam->get(),
                                           customSamplesParam->get(),
                                           sampleRate_);
    delayLine.setDelayFrames (frames);                           // hard jump when it changes
    currentDelayFrames.store (delayLine.getDelayFrames());
}

double PitchDelayProcessor::getTailLengthSeconds() const
{
    return sampleRate_ > 0.0 ? (double) getCurrentDelayFrames() / sampleRate_ : 0.0;
}

void PitchDelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Deliberately NO juce::ScopedNoDenormals: the output must stay bit-identical to the input.
    if (bypassParam->get())
    {
        processBlockBypassed (buffer, midi);
        return;
    }
    updateDelay();
    delayLine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void PitchDelayProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Output stays the input (undelayed); keep recording so un-bypass is seamless.
    updateDelay();
    delayLine.record (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void PitchDelayProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PitchDelayProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    if (const auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}
