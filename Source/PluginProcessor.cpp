#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "core.h"
#include "BinaryData.h"
#include <vector>

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
    // 1) Load test2.wav from BinaryData into a juce::AudioBuffer<float>
    //    (reuses JUCE's own WAV reader no re-implementation needed).
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    auto inputStream = std::make_unique<juce::MemoryInputStream>(
        BinaryData::test2_wav, (size_t)BinaryData::test2_wavSize, false);

    std::unique_ptr<juce::AudioFormatReader> reader(
        formatManager.createReaderFor(std::move(inputStream)));

    if (reader == nullptr)
    {
        DBG("runTestTranscription: failed to create reader for embedded test2.wav");
        return;
    }

    juce::AudioBuffer<float> fileBuffer((int)reader->numChannels, (int)reader->lengthInSamples);
    reader->read(&fileBuffer, 0, (int)reader->lengthInSamples, 0, true, true);

    // 2) Populate a C_FloatArray (interleaved, as core.c expects) from the AudioBuffer<float>
    const int numChannels = fileBuffer.getNumChannels();
    const int numSamples = fileBuffer.getNumSamples();

    std::vector<float> interleaved((size_t)numChannels * (size_t)numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* src = fileBuffer.getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i)
            interleaved[(size_t)i * (size_t)numChannels + (size_t)ch] = src[i];
    }

    C_FloatArray audio;
    audio.data = interleaved.data();
    audio.num_samples = (int64_t)numSamples;
    audio.num_channels = numChannels;
    audio.sample_rate = (int)reader->sampleRate;

    // 3) Process the C_FloatArray in core.c, identical downmix/resample/inference
    //    pipeline that used to run on the file loaded from disk.
    ppd_run_test_buffer(&audio);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PPDAudioProcessor();
}
