#include "PluginProcessor.h"

using namespace dragtime;

DragTimeProcessor::DragTimeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    power   = apvts.getRawParameterValue ("power");
    speed   = apvts.getRawParameterValue ("speed");
    length  = apvts.getRawParameterValue ("length");
    mix     = apvts.getRawParameterValue ("mix");
    smooth  = apvts.getRawParameterValue ("smooth");
    bandLo  = apvts.getRawParameterValue ("bandLo");
    bandHi  = apvts.getRawParameterValue ("bandHi");
    fadeIn  = apvts.getRawParameterValue ("fadeIn");
    fadeOut = apvts.getRawParameterValue ("fadeOut");
    filterLo  = apvts.getRawParameterValue ("filterLo");
    filterHi  = apvts.getRawParameterValue ("filterHi");
    filterRes = apvts.getRawParameterValue ("filterRes");
    djFilter  = apvts.getRawParameterValue ("djFilter");
    lfoDepth  = apvts.getRawParameterValue ("lfoDepth");
    lfoRate   = apvts.getRawParameterValue ("lfoRate");
    lfoShape  = apvts.getRawParameterValue ("lfoShape");
    drive     = apvts.getRawParameterValue ("drive");
}

juce::AudioProcessorValueTreeState::ParameterLayout DragTimeProcessor::createLayout()
{
    using namespace juce;

    // The choice lists are built from the tables in Engine.h, so adding a
    // speed or loop length there is all that is needed.
    StringArray speedNames, lengthNames, lfoRateNames, lfoShapeNames;
    for (const auto& m : kSpeedModes)  speedNames.add (m.name);
    for (const auto& l : kLoopLengths) lengthNames.add (l.name);
    for (const auto& r : kLfoRates)    lfoRateNames.add (r.name);
    for (const auto* n : kLfoShapes)   lfoShapeNames.add (n);

    NormalisableRange<float> hzRange (20.0f, 20000.0f, 0.0f, 0.25f);
    NormalisableRange<float> msRange (0.0f, 2000.0f, 0.0f, 0.4f);

    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { "power", 1 },  "Power", true));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "speed", 1 },  "Speed", speedNames, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "length", 1 }, "Loop Length", lengthNames, 4));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "mix", 1 },    "Mix", 0.0f, 1.0f, 1.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "smooth", 1 }, "Smooth", 0.0f, 1.0f, 0.3f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "bandLo", 1 }, "Band Low (Hz)", hzRange, 20.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "bandHi", 1 }, "Band High (Hz)", hzRange, 20000.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "fadeIn", 1 }, "Fade In (ms)", msRange, 20.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "fadeOut", 1 }, "Fade Out (ms)", msRange, 20.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterLo", 1 },  "Filter Low Cut (Hz)", hzRange, 20.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterHi", 1 },  "Filter High Cut (Hz)", hzRange, 20000.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "filterRes", 1 }, "Filter Resonance", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "djFilter", 1 },  "DJ Filter (LP < 0 > HP)", -1.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "lfoDepth", 1 },  "Filter LFO Depth", 0.0f, 1.0f, 0.0f));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "lfoRate", 1 },   "Filter LFO Rate", lfoRateNames, 3));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "lfoShape", 1 },  "Filter LFO Shape", lfoShapeNames, 0));
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { "drive", 1 },     "Drive", 0.0f, 1.0f, 0.0f));
    return layout;
}

void DragTimeProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, getTotalNumInputChannels());
}

bool DragTimeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void DragTimeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    double ppq = 0.0;
    bool playing = false;

    if (auto* playHead = getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (auto bpm = position->getBpm())         lastBpm = *bpm;
            if (auto p   = position->getPpqPosition()) ppq = *p;
            playing = position->getIsPlaying();
        }
    }

    const auto speedIndex  = (size_t) juce::jlimit (0, (int) kSpeedModes.size() - 1,  (int) speed->load());
    const auto lengthIndex = (size_t) juce::jlimit (0, (int) kLoopLengths.size() - 1, (int) length->load());

    Params p;
    p.power     = power->load() > 0.5f;
    p.rate      = kSpeedModes[speedIndex].rate;
    p.loopBeats = kLoopLengths[lengthIndex].beats;
    p.mix       = mix->load();
    p.smooth    = smooth->load();
    p.bandLoHz  = bandLo->load();
    p.bandHiHz  = bandHi->load();
    p.fadeInMs  = fadeIn->load();
    p.fadeOutMs = fadeOut->load();
    p.filterLoHz = filterLo->load();
    p.filterHiHz = filterHi->load();
    p.filterRes  = filterRes->load();
    p.djFilter   = djFilter->load();
    p.drive      = drive->load();
    p.lfoDepth   = lfoDepth->load();
    p.lfoShape   = juce::jlimit (0, (int) kLfoShapes.size() - 1, (int) lfoShape->load());
    p.lfoBeats   = kLfoRates[(size_t) juce::jlimit (0, (int) kLfoRates.size() - 1, (int) lfoRate->load())].beats;

    engine.process (buffer.getArrayOfWritePointers(),
                    juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels()),
                    buffer.getNumSamples(), p, lastBpm, ppq, playing);
}

juce::AudioProcessorEditor* DragTimeProcessor::createEditor()
{
    // Auto-generated sliders for every parameter. Replace with your own
    // AudioProcessorEditor subclass when you want a custom interface.
    return new juce::GenericAudioProcessorEditor (*this);
}

void DragTimeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DragTimeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DragTimeProcessor();
}
