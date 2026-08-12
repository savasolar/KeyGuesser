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
    // Block until the background LambdaThread has finished.
    // (prevents the leaked LambdaThread assertion and use-after-free)
    while (isTranscribing.load(std::memory_order_acquire))
        juce::Thread::sleep(5);

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
    currentSampleRate = sampleRate;
    contextSamples = (int)(contextLengthSeconds * sampleRate + 0.5);

    const int numCh = juce::jmax(1, getTotalNumInputChannels());
    contextBuffer.setSize(numCh, contextSamples, false, true, true); // clear, avoid realloc
    writePosition = 0;
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

void PPDAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    if (contextSamples <= 0 || contextBuffer.getNumSamples() != contextSamples)
        return;

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin(totalNumInputChannels, contextBuffer.getNumChannels());

    // 1. Write live audio into the ring buffer
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* src = buffer.getReadPointer(ch);
        float* dest = contextBuffer.getWritePointer(ch);
        int          pos = writePosition;

        for (int i = 0; i < numSamples; ++i)
        {
            dest[pos] = src[i];
            if (++pos >= contextSamples)
                pos = 0;
        }
    }
    writePosition = (writePosition + numSamples) % contextSamples;

    // 2. Count samples. When a full contextLengthSeconds has arrived, fire.
    samplesSinceLast += numSamples;
    if (samplesSinceLast >= contextSamples)
    {
        samplesSinceLast -= contextSamples;   // keep residual for accuracy

        // Never block the audio thread. Launch once, skip if already running.
        if (!isTranscribing.exchange(true))
        {
            // --- snapshot on the audio thread (no race) ---
            juce::AudioBuffer<float> snapshot(contextBuffer.getNumChannels(), contextSamples);
            const int numCh = contextBuffer.getNumChannels();

            for (int ch = 0; ch < numCh; ++ch)
            {
                const float* src = contextBuffer.getReadPointer(ch);
                float* dest = snapshot.getWritePointer(ch);

                const int first = contextSamples - writePosition;
                if (first > 0)
                    juce::FloatVectorOperations::copy(dest, src + writePosition, first);
                if (writePosition > 0)
                    juce::FloatVectorOperations::copy(dest + first, src, writePosition);
            }

            const double sr = currentSampleRate;   // capture by value

            juce::Thread::launch([this, snapshot = std::move(snapshot), sr]
                {
                    transcribeAudioBuffer(snapshot, sr);
                    isTranscribing.store(false, std::memory_order_release);
                });
        }
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

void PPDAudioProcessor::transcribeAudioBuffer(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    std::vector<float> interleaved((size_t)numChannels * (size_t)numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* src = buffer.getReadPointer(ch);
        for (int i = 0; i < numSamples; ++i)
            interleaved[(size_t)i * (size_t)numChannels + (size_t)ch] = src[i];
    }

    C_FloatArray audio;
    audio.data = interleaved.data();
    audio.num_samples = (int64_t)numSamples;
    audio.num_channels = numChannels;
    audio.sample_rate = (int)sampleRate;

    C_PitchResult pitchResult;
    pitchResult.num_pitches = 0;
    ppd_run_test_buffer(&audio, &pitchResult);

    // Only overwrite detectedNotes when this transcription actually found
    // something - an empty result just means "nothing new", so keep
    // showing the last known notes rather than blanking the UI.
    if (pitchResult.num_pitches > 0)
    {
        const juce::ScopedLock sl(detectedNotesLock);
        detectedNotes.assign(pitchResult.pitches, pitchResult.pitches + pitchResult.num_pitches);
    }
}

std::vector<int> PPDAudioProcessor::getDetectedNotes() const
{
    const juce::ScopedLock sl(detectedNotesLock);
    return detectedNotes;
}

void PPDAudioProcessor::runTestTranscription()
{
    // 1) Load test2.wav from BinaryData (unchanged)
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

    juce::AudioBuffer<float> fileBuffer((int)reader->numChannels,
        (int)reader->lengthInSamples);
    reader->read(&fileBuffer, 0, (int)reader->lengthInSamples, 0, true, true);

    // 2) Re-use the exact same interleave + core path
    transcribeAudioBuffer(fileBuffer, reader->sampleRate);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PPDAudioProcessor();
}
