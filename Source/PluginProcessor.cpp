#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "core.h"
#include "BinaryData.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <thread>

namespace
{
    constexpr const char* NOTE_NAMES[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    // Krumhansl & Kessler (1982) probe-tone profiles, as published in
    // Krumhansl, C. L. (1990) "Cognitive Foundations of Musical Pitch",
    // Oxford University Press, Table 2. Index 0 = tonic, index 1 = tonic + 1
    // semitone, etc. Public, widely-republished research values (also reused
    // as-is in music21, Essentia, miditoolbox) - see key_estimator.py.
    constexpr std::array<double, 12> MAJOR_PROFILE = {
        6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
        2.52, 5.19, 2.39, 3.66, 2.29, 2.88
    };
    constexpr std::array<double, 12> MINOR_PROFILE = {
        6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
        2.54, 4.75, 3.98, 2.69, 3.34, 3.17
    };

    // Fold MIDI notes (any octave) into a 12-bin pitch-class histogram.
    // Equal weight per note, matching key_estimator.py's default.
    std::array<double, 12> notesToPitchClassHistogram(const std::vector<int>& midiNotes)
    {
        std::array<double, 12> histogram{};
        histogram.fill(0.0);
        for (int note : midiNotes)
        {
            const int pc = ((note % 12) + 12) % 12;
            histogram[(size_t)pc] += 1.0;
        }
        return histogram;
    }

    // Pearson correlation between the observed histogram and `profile`
    // rotated so its tonic sits at pitch class `tonic` (mirrors
    // np.roll(profile, tonic) + np.corrcoef in key_estimator.py).
    double correlate(const std::array<double, 12>& histogram, int tonic,
        const std::array<double, 12>& profile)
    {
        std::array<double, 12> rotated{};
        for (int i = 0; i < 12; ++i)
            rotated[(size_t)((tonic + i) % 12)] = profile[(size_t)i];

        const double histMean = std::accumulate(histogram.begin(), histogram.end(), 0.0) / 12.0;
        const double profMean = std::accumulate(rotated.begin(), rotated.end(), 0.0) / 12.0;

        double num = 0.0, histSS = 0.0, profSS = 0.0;
        for (int i = 0; i < 12; ++i)
        {
            const double hd = histogram[(size_t)i] - histMean;
            const double pd = rotated[(size_t)i] - profMean;
            num += hd * pd;
            histSS += hd * hd;
            profSS += pd * pd;
        }

        if (histSS == 0.0 || profSS == 0.0)
            return 0.0;

        return num / std::sqrt(histSS * profSS);
    }

    // Exact replica of key_estimator.py's estimate_key() (Krumhansl-Schmuckler)
    std::vector<std::pair<juce::String, double>>
        estimateKey(const std::vector<int>& midiNotes, int topN = 3)
    {
        if (midiNotes.empty())
            return {};

        const auto histogram = notesToPitchClassHistogram(midiNotes);

        std::vector<std::pair<juce::String, double>> scores;
        scores.reserve(24);

        for (int tonic = 0; tonic < 12; ++tonic)
        {
            const juce::String majName = juce::String(NOTE_NAMES[tonic]) + "maj";
            const juce::String minName = juce::String(NOTE_NAMES[tonic]) + "min";

            scores.emplace_back(majName, correlate(histogram, tonic, MAJOR_PROFILE));
            scores.emplace_back(minName, correlate(histogram, tonic, MINOR_PROFILE));
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
    // Models live next to the plugin binary itself
    juce::File pluginDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getParentDirectory();

    juce::String prefillPath = pluginDir.getChildFile("prefill.onnx").getFullPathName();
    juce::String stepPath = pluginDir.getChildFile("step.onnx").getFullPathName();

    ppd_load_models(prefillPath.toRawUTF8(), stepPath.toRawUTF8());
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

            std::thread([this, snapshot = std::move(snapshot), sr]() mutable
                {
                    transcribeAudioBuffer(snapshot, sr);
                    isTranscribing.store(false, std::memory_order_release);
                }).detach();
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
