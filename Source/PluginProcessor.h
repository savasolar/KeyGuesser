#pragma once

#include <JuceHeader.h>
#include "core.h"

class PPDAudioProcessor  : public juce::AudioProcessor
{
public:
    PPDAudioProcessor();
    ~PPDAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void runTestTranscription();

private:

    int contextLengthSeconds = 5;

    double currentSampleRate = 44100.0;
    juce::AudioBuffer<float> contextBuffer;
    int writePosition = 0;
    int contextSamples = 0;

    int samplesSinceLast = 0;

    std::atomic<bool> isTranscribing{ false };

    /** Shared path used by both the BinaryData test and the live context buffer.
     *  Interleaves the (planar) AudioBuffer and calls ppd_run_test_buffer. */
    void transcribeAudioBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PPDAudioProcessor)
};
