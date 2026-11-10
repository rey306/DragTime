#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"

class DragTimeProcessor : public juce::AudioProcessor
{
public:
    DragTimeProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "DragTime"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    dragtime::Engine engine;
    double lastBpm = 120.0;

    std::atomic<float>* power   = nullptr;
    std::atomic<float>* speed   = nullptr;
    std::atomic<float>* length  = nullptr;
    std::atomic<float>* mix     = nullptr;
    std::atomic<float>* smooth  = nullptr;
    std::atomic<float>* bandLo  = nullptr;
    std::atomic<float>* bandHi  = nullptr;
    std::atomic<float>* fadeIn  = nullptr;
    std::atomic<float>* fadeOut = nullptr;
    std::atomic<float>* filterLo  = nullptr;
    std::atomic<float>* filterHi  = nullptr;
    std::atomic<float>* filterRes = nullptr;
    std::atomic<float>* djFilter  = nullptr;
    std::atomic<float>* lfoDepth  = nullptr;
    std::atomic<float>* lfoRate   = nullptr;
    std::atomic<float>* lfoShape  = nullptr;
    std::atomic<float>* drive     = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DragTimeProcessor)
};
