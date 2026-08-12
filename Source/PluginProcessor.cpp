#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "core.h"
#include "BinaryData.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>

namespace
{
    using Vec3 = std::array<double, 3>;

    constexpr double R = 1.0;
    const double H = std::sqrt(2.0 / 15.0);

    constexpr std::array<double, 3> W_MAJOR = { 0.516, 0.315, 0.168 };
    constexpr std::array<double, 3> W_MINOR = { 0.516, 0.315, 0.168 };
    constexpr std::array<double, 3> OMEGA = { 0.516, 0.315, 0.168 };
    constexpr std::array<double, 3> NU = { 0.516, 0.315, 0.168 };
    constexpr double ALPHA = 1.0;
    constexpr double BETA = 0.0;

    constexpr const char* NOTE_NAMES[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    // Pitch-class → index on the line of fifths (C=0 … F=−1)
    constexpr int PC_TO_K[12] = {
        0,  // C
        7,  // C#
        2,  // D
        9,  // D#
        4,  // E
       -1,  // F
        6,  // F#
        1,  // G
        8,  // G#
        3,  // A
       10,  // A#
        5   // B
    };

    constexpr double PI = 3.14159265358979323846;

    Vec3 P(double k)
    {
        return {
            R * std::sin(k * PI / 2.0),
            R * std::cos(k * PI / 2.0),
            k * H
        };
    }

    Vec3 scale(const Vec3& v, double s)
    {
        return { v[0] * s, v[1] * s, v[2] * s };
    }

    Vec3 add(const Vec3& a, const Vec3& b)
    {
        return { a[0] + b[0], a[1] + b[1], a[2] + b[2] };
    }

    Vec3 CM(int k)   // major chord CE
    {
        return add(add(scale(P(k), W_MAJOR[0]),
            scale(P(k + 1), W_MAJOR[1])),
            scale(P(k + 4), W_MAJOR[2]));
    }

    Vec3 Cm(int k)   // minor chord CE
    {
        return add(add(scale(P(k), W_MINOR[0]),
            scale(P(k + 1), W_MINOR[1])),
            scale(P(k - 3), W_MINOR[2]));
    }

    Vec3 TM(int k)   // major key
    {
        return add(add(scale(CM(k), OMEGA[0]),
            scale(CM(k + 1), OMEGA[1])),
            scale(CM(k - 1), OMEGA[2]));
    }

    Vec3 Tm(int k)   // minor key (ALPHA/BETA mixture)
    {
        const Vec3 V = add(scale(CM(k + 1), ALPHA),
            scale(Cm(k + 1), 1.0 - ALPHA));
        const Vec3 iv = add(scale(Cm(k - 1), BETA),
            scale(CM(k - 1), 1.0 - BETA));
        return add(add(scale(Cm(k), NU[0]),
            scale(V, NU[1])),
            scale(iv, NU[2]));
    }

    double dist(const Vec3& a, const Vec3& b)
    {
        const double dx = a[0] - b[0];
        const double dy = a[1] - b[1];
        const double dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Exact replica of spiral_array.estimate_key
    std::vector<std::pair<juce::String, double>>
        estimateKey(const std::vector<int>& midiNotes, int topN = 3)
    {
        if (midiNotes.empty())
            return {};

        // Centre of effect (equal weight per note)
        Vec3 ce{ 0.0, 0.0, 0.0 };
        for (int note : midiNotes)
        {
            const int pc = ((note % 12) + 12) % 12;
            ce = add(ce, P(PC_TO_K[pc]));
        }
        ce = scale(ce, 1.0 / static_cast<double>(midiNotes.size()));

        std::vector<std::pair<juce::String, double>> scores;
        scores.reserve(24);

        for (int tonic = 0; tonic < 12; ++tonic)
        {
            const int k = PC_TO_K[tonic];
            const juce::String majName = juce::String(NOTE_NAMES[tonic]) + "maj";
            const juce::String minName = juce::String(NOTE_NAMES[tonic]) + "min";

            scores.emplace_back(majName, -dist(ce, TM(k)));
            scores.emplace_back(minName, -dist(ce, Tm(k)));
        }

        std::sort(scores.begin(), scores.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        if (static_cast<int>(scores.size()) > topN)
            scores.resize(static_cast<size_t>(topN));

        return scores;
    }
} // namespace

PPDAudioProcessor::PPDAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
    apvts(*this, nullptr, "Parameters", createParams())
#endif
{
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

void PPDAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    currentSampleRate = sampleRate;

    constexpr int maxSeconds = 10;
    const int maxSamples = (int)(maxSeconds * sampleRate + 0.5);
    const int numCh = juce::jmax(1, getTotalNumInputChannels());

    // Allocate once for the largest possible context. Never touch setSize again on the audio thread.
    contextBuffer.setSize(numCh, maxSamples, false, true, true);

    contextSamples = (int)(getContextLengthSeconds() * sampleRate + 0.5);
    if (contextSamples > maxSamples)
        contextSamples = maxSamples;

    writePosition = 0;
    samplesSinceLast = 0;
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

void PPDAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;
    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // CHANGED: buffer is now always max-size, so test against the logical length
    if (contextSamples <= 0 || contextBuffer.getNumSamples() < contextSamples)
        return;

    // ------------------------------------------------------------------
    // SAFE PARAMETER UPDATE (only when not mid-transcription)
    // ------------------------------------------------------------------
    {
        const int desired = (int)(getContextLengthSeconds() * currentSampleRate + 0.5);

        if (desired != contextSamples
            && desired > 0
            && desired <= contextBuffer.getNumSamples()
            && !isTranscribing.load(std::memory_order_acquire))
        {
            contextSamples = desired;
            if (writePosition >= contextSamples)
                writePosition = 0;
            samplesSinceLast = 0;          // start a clean full window
        }
    }
    // ------------------------------------------------------------------

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin(totalNumInputChannels, contextBuffer.getNumChannels());

    // 1. Write live audio into the ring buffer (still uses the *logical* contextSamples)
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* src = buffer.getReadPointer(ch);
        float* dest = contextBuffer.getWritePointer(ch);
        int pos = writePosition;

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
        samplesSinceLast -= contextSamples;

        if (!isTranscribing.exchange(true))
        {
            juce::AudioBuffer<float> snapshot(contextBuffer.getNumChannels(), contextSamples);
            const int nCh = contextBuffer.getNumChannels();

            for (int ch = 0; ch < nCh; ++ch)
            {
                const float* src = contextBuffer.getReadPointer(ch);
                float* dest = snapshot.getWritePointer(ch);

                const int first = contextSamples - writePosition;
                if (first > 0)
                    juce::FloatVectorOperations::copy(dest, src + writePosition, first);
                if (writePosition > 0)
                    juce::FloatVectorOperations::copy(dest + first, src, writePosition);
            }

            const double sr = currentSampleRate;

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

void PPDAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void PPDAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
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
    /*if (pitchResult.num_pitches > 0)
    {
        const juce::ScopedLock sl(detectedNotesLock);
        detectedNotes.assign(pitchResult.pitches, pitchResult.pitches + pitchResult.num_pitches);
        estimatedKeys = estimateKey(detectedNotes, 3);
    }*/

    {
        const juce::ScopedLock sl(detectedNotesLock);
        detectedNotes.assign(pitchResult.pitches, pitchResult.pitches + pitchResult.num_pitches);
        estimatedKeys = estimateKey(detectedNotes, 3);
    }
}

std::vector<int> PPDAudioProcessor::getDetectedNotes() const
{
    const juce::ScopedLock sl(detectedNotesLock);
    return detectedNotes;
}

std::vector<std::pair<juce::String, double>> PPDAudioProcessor::getEstimatedKeys() const
{
    const juce::ScopedLock sl(detectedNotesLock);
    return estimatedKeys;
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

juce::AudioProcessorValueTreeState::ParameterLayout PPDAudioProcessor::createParams()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterInt>("ContextLength", "Context Length", 2, 10, 5));
    return { params.begin(), params.end() };
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PPDAudioProcessor();
}
