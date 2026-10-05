// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 ameisevinyl

#include "PluginProcessor.h"

#include "DelaySettings.h"
#include "ParameterIds.h"

using namespace pitchdelay;

namespace
{
const juce::Identifier delayFramesProperty { "delayFrames" };
const juce::Identifier delayRateProperty { "delayRate" };
const juce::Identifier delayTunedProperty { "delayTuned" };
const juce::Identifier anchorFramesProperty { "delayAnchorFrames" };
const juce::Identifier anchorRateProperty { "delayAnchorRate" };

struct LegacyCustom
{
    bool isCustom = false;
    bool inMilliseconds = true;
    double milliseconds = 0.0;
    int samples = 0;
};

double paramValue (const juce::ValueTree& tree, const char* id, double fallback)
{
    const auto child = tree.getChildWithProperty ("id", id);
    return child.isValid() ? (double) child.getProperty ("value", fallback) : fallback;
}

// Reads the first-release Custom settings from a saved state tree (mode 2 = Custom).
LegacyCustom readLegacyCustom (const juce::ValueTree& tree)
{
    LegacyCustom l;
    l.isCustom = juce::roundToInt (paramValue (tree, ids::mode, 0.0)) == 2;
    l.inMilliseconds = juce::roundToInt (paramValue (tree, ids::legacyCustomUnit, 0.0)) == 0;
    l.milliseconds = paramValue (tree, ids::legacyCustomMs, 900.0);
    l.samples = juce::roundToInt (paramValue (tree, ids::legacyCustomSamples, 0.0));
    return l;
}

// Drops the removed parameters so they are not saved again; Custom becomes the first speed.
void stripLegacy (juce::ValueTree tree, bool wasCustom)
{
    for (const char* id : { ids::legacyCustomUnit, ids::legacyCustomMs, ids::legacyCustomSamples })
    {
        auto child = tree.getChildWithProperty ("id", id);
        if (child.isValid())
            tree.removeChild (child, nullptr);
    }
    if (wasCustom)
    {
        auto mode = tree.getChildWithProperty ("id", ids::mode);
        if (mode.isValid())
            mode.setProperty ("value", 0, nullptr);
    }
}
}

juce::AudioProcessorValueTreeState::ParameterLayout PitchDelayProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::mode, 2 }, "Speed",
        StringArray { "33 1/3 RPM", "45 RPM" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ids::fraction, 1 }, "Fraction",
        StringArray { "1/1", "1/2", "1/4", "1/8", "1/16" }, kDefaultFractionIndex,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterBool> (
        ParameterID { ids::calibration, 1 }, "Calibration", false,
        AudioParameterBoolAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ids::calibrationLevel, 1 }, "Calibration level",
        NormalisableRange<float> (-60.0f, 0.0f, 0.5f), -20.0f,
        AudioParameterFloatAttributes().withLabel ("dB").withAutomatable (false)));

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
    fractionParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ids::fraction));
    calibrationParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ids::calibration));
    calibrationLevelParam = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (ids::calibrationLevel));
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ids::bypass));
    jassert (modeParam && fractionParam && calibrationParam && calibrationLevelParam && bypassParam);

    apvts.addParameterListener (ids::mode, this);
    apvts.addParameterListener (ids::fraction, this);
}

PitchDelayProcessor::~PitchDelayProcessor()
{
    apvts.removeParameterListener (ids::mode, this);
    apvts.removeParameterListener (ids::fraction, this);
}

bool PitchDelayProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return out == layouts.getMainInputChannelSet();
}

void PitchDelayProcessor::reset()
{
    delayLine.reset();
    pulse.reset();
    calibrating = false;
}

int PitchDelayProcessor::baseDelayAt (double sampleRate) const
{
    return baseDelayFrames (static_cast<Speed> (modeParam->getIndex()), fractionParam->getIndex(), sampleRate);
}

void PitchDelayProcessor::applyBaseDelay()
{
    // Speed or Fraction chosen (or an untouched delay at a new rate): exact value, no fine-tuning.
    const juce::ScopedLock lock (delayStateLock);
    delayTuned = false;
    pendingLegacyMs = -1.0;
    const double sr = sampleRate_.load();
    if (sr > 0.0)
        delayFrames.store (baseDelayAt (sr));
}

void PitchDelayProcessor::parameterChanged (const juce::String&, float)
{
    // Speed or Fraction changed: the delay follows them (overwriting any fine-tuning).
    applyBaseDelay();
}

void PitchDelayProcessor::setDelayFrames (int frames)
{
    const juce::ScopedLock lock (delayStateLock);
    const double sr = sampleRate_.load();
    if (sr <= 0.0)
        return;
    anchorFrames = clampFrames (frames, sr);
    anchorRate = sr;
    delayTuned = true;
    pendingLegacyMs = -1.0;
    delayFrames.store (anchorFrames);
}

void PitchDelayProcessor::prepareToPlay (double sampleRate, int)
{
    {
        const juce::ScopedLock lock (delayStateLock);
        sampleRate_.store (sampleRate);

        if (pendingLegacyMs >= 0.0)
        {
            anchorFrames = clampFrames (framesForMilliseconds (pendingLegacyMs, sampleRate), sampleRate);
            anchorRate = sampleRate;
            delayTuned = true;
            pendingLegacyMs = -1.0;
        }

        if (delayTuned)
        {
            if (anchorRate <= 0.0)
                anchorRate = sampleRate;   // frames restored before the rate was known
            delayFrames.store (rescaleFrames (anchorFrames, anchorRate, sampleRate));
        }
        else
        {
            applyBaseDelay();
        }
    }

    const int channels = std::max ({ 1, getTotalNumInputChannels(), getTotalNumOutputChannels() });
    delayLine.prepare (channels, maxDelayFrames (sampleRate));   // allocates and clears
    pulse.prepare (sampleRate);
    calibrating = false;
    updateDsp();
}

double PitchDelayProcessor::getTailLengthSeconds() const
{
    const double sr = sampleRate_.load();
    return sr > 0.0 ? static_cast<double> (getCurrentDelayFrames()) / sr : 0.0;
}

void PitchDelayProcessor::updateDsp()
{
    const int frames = delayFrames.load();
    delayLine.setDelayFrames (frames);
    pulse.setPeriodFrames (frames);
    pulse.setLevelDb (calibrationLevelParam->get());
}

bool PitchDelayProcessor::hostIsStopped() const
{
    // Hosts without transport information (e.g. Standalone) are treated as always playing.
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
            return ! position->getIsPlaying();
    return false;
}

void PitchDelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Deliberately NO juce::ScopedNoDenormals: the output must stay bit-identical to the input.
    if (bypassParam->get())
    {
        processBlockBypassed (buffer, midi);
        return;
    }
    if (hostIsStopped())
    {
        // Nothing may survive a stop: the next start must begin with a full delay of silence.
        delayLine.reset();
        pulse.reset();
        calibrating = false;
        buffer.clear();
        return;
    }

    updateDsp();
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (calibrationParam->get() && numChannels > 0)
    {
        if (! calibrating)
        {
            pulse.reset();   // switched on: the first pulse starts at this block's first frame
            calibrating = true;
        }
        // Keep recording the incoming audio so leaving calibration is seamless.
        delayLine.record (buffer.getArrayOfReadPointers(), numChannels, numSamples);
        float* first = buffer.getWritePointer (0);
        pulse.generate (first, numSamples);
        for (int c = 1; c < numChannels; ++c)
            std::copy (first, first + numSamples, buffer.getWritePointer (c));
        return;
    }

    calibrating = false;
    delayLine.process (buffer.getArrayOfWritePointers(), numChannels, numSamples);
}

void PitchDelayProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Output stays the input (undelayed); keep recording so un-bypass is seamless,
    // except while the host is stopped, when the buffer and the pulse phase are wiped instead.
    if (hostIsStopped())
    {
        delayLine.reset();
        pulse.reset();
        calibrating = false;
        return;
    }
    updateDsp();
    delayLine.record (buffer.getArrayOfReadPointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

void PitchDelayProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    const juce::ScopedLock lock (delayStateLock);
    state.setProperty (delayFramesProperty, delayFrames.load(), nullptr);
    state.setProperty (delayRateProperty, sampleRate_.load(), nullptr);
    state.setProperty (delayTunedProperty, delayTuned, nullptr);
    if (delayTuned)
    {
        state.setProperty (anchorFramesProperty, anchorFrames, nullptr);
        state.setProperty (anchorRateProperty, anchorRate, nullptr);
    }
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PitchDelayProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    const auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);

    // Read what we need before the tree is handed to the parameters.
    const bool hasDelay = tree.hasProperty (delayFramesProperty) && tree.hasProperty (delayRateProperty);
    const bool hasTunedFlag = tree.hasProperty (delayTunedProperty);
    const bool storedTuned = (bool) tree.getProperty (delayTunedProperty, false);
    const int storedFrames = juce::jmax (0, (int) tree.getProperty (delayFramesProperty, 0));
    const double storedRate = (double) tree.getProperty (delayRateProperty, 0.0);
    const int storedAnchorFrames = juce::jmax (0, (int) tree.getProperty (anchorFramesProperty, 0));
    const double storedAnchorRate = (double) tree.getProperty (anchorRateProperty, 0.0);
    const auto legacy = readLegacyCustom (tree);
    stripLegacy (tree, legacy.isCustom);

    apvts.replaceState (tree);   // may run the Speed/Fraction listener; resolved explicitly below

    const juce::ScopedLock lock (delayStateLock);   // sampleRate_ and the stores below stay consistent
    const double sr = sampleRate_.load();
    pendingLegacyMs = -1.0;

    if (hasTunedFlag && storedTuned)
    {
        // This build: a tuned delay with its anchor.
        delayTuned = true;
        anchorFrames = storedAnchorFrames;
        anchorRate = storedAnchorRate;
        delayFrames.store (sr > 0.0 ? rescaleFrames (anchorFrames, anchorRate > 0.0 ? anchorRate : sr, sr)
                                    : anchorFrames);
    }
    else if (hasTunedFlag)
    {
        // This build: an untouched delay, recomputed from Speed and Fraction.
        delayTuned = false;
        if (sr > 0.0)
            delayFrames.store (baseDelayAt (sr));
    }
    else if (hasDelay && storedRate > 0.0)
    {
        // The previous build (frames + rate only) did not remember how a delay was set: keep it as tuned.
        delayTuned = true;
        anchorFrames = storedFrames;
        anchorRate = storedRate;
        delayFrames.store (sr > 0.0 ? rescaleFrames (anchorFrames, anchorRate, sr) : anchorFrames);
    }
    else if (legacy.isCustom && legacy.inMilliseconds)
    {
        delayTuned = false;
        if (sr > 0.0)
        {
            delayTuned = true;
            anchorFrames = clampFrames (framesForMilliseconds (legacy.milliseconds, sr), sr);
            anchorRate = sr;
            delayFrames.store (anchorFrames);
        }
        else
        {
            pendingLegacyMs = juce::jmax (0.0, legacy.milliseconds);   // resolved in prepareToPlay
        }
    }
    else if (legacy.isCustom)
    {
        delayTuned = true;
        anchorFrames = sr > 0.0 ? clampFrames (legacy.samples, sr) : juce::jmax (0, legacy.samples);
        anchorRate = sr;   // 0 before prepare: adopted when the rate becomes known
        delayFrames.store (anchorFrames);
    }
    else
    {
        delayTuned = false;   // first-release 33 1/3 or 45 RPM
        if (sr > 0.0)
            delayFrames.store (baseDelayAt (sr));
    }
}
