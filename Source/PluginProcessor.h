#pragma once

#include <JuceHeader.h>
#include "core.h"
#include <utility>

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

    /** Thread-safe accessor for the UI: returns a copy of the unique MIDI
     *  pitches from the most recent NON-EMPTY transcription. Safe to call
     *  from the message thread while transcribeAudioBuffer() updates
     *  detectedNotes on a background thread. */
    std::vector<int> getDetectedNotes() const;

    /** Thread-safe top-3 key estimates (name + score) produced by the
     *  Krumhansl-Schmuckler key-profile correlation from the same notes.
     *  Score is a Pearson correlation coefficient (range -1..1); higher =
     *  more likely. Empty when no notes have been detected yet. */
    std::vector<std::pair<juce::String, double>> getEstimatedKeys() const;

    juce::AudioProcessorValueTreeState apvts;

    int getContextLengthSeconds() const
    {
        return juce::jlimit(2, 10, (int)*apvts.getRawParameterValue("ContextLength"));
    }

private:

//    int contextLengthSeconds = 5;

    double currentSampleRate = 44100.0;
    juce::AudioBuffer<float> contextBuffer;
    int writePosition = 0;
    int contextSamples = 0;
    int samplesSinceLast = 0;

    std::atomic<bool> isTranscribing{ false };
    mutable juce::CriticalSection detectedNotesLock;
    std::vector<int> detectedNotes;
    std::vector<std::pair<juce::String, double>> estimatedKeys;

    juce::AudioProcessorValueTreeState::ParameterLayout createParams();

    void transcribeAudioBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PPDAudioProcessor)
};
