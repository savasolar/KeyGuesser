#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "core.h"

PPDAudioProcessor::PPDAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
//    run_ppd_transcription();
    ppd_load_models();

}

PPDAudioProcessor::~PPDAudioProcessor()
{
    ppd_shutdown();
}

const juce::String PPDAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PPDAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PPDAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PPDAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double PPDAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PPDAudioProcessor::getNumPrograms()
{
    return 1;
}

int PPDAudioProcessor::getCurrentProgram()
{
    return 0;
}

void PPDAudioProcessor::setCurrentProgram (int index)
{
}

const juce::String PPDAudioProcessor::getProgramName (int index)
{
    return {};
}

void PPDAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

void PPDAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{

}

void PPDAudioProcessor::releaseResources()
{

}

#ifndef JucePlugin_PreferredChannelConfigurations
bool PPDAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else

    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void PPDAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {
        auto* channelData = buffer.getWritePointer (channel);

    }
}

bool PPDAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* PPDAudioProcessor::createEditor()
{
    return new PPDAudioProcessorEditor (*this);
}

void PPDAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{

}

void PPDAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{

}

void PPDAudioProcessor::runTestTranscription()
{
    ppd_run_test();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PPDAudioProcessor();
}
