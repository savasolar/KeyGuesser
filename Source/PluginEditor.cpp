#include "PluginProcessor.h"
#include "PluginEditor.h"

PPDAudioProcessorEditor::PPDAudioProcessorEditor(PPDAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{

    keyTitleLabel.setText("KEY ESTIMATES:", juce::dontSendNotification);
    keyTitleLabel.setFont(juce::Font("Arial", 14.0f, juce::Font::plain));
    keyTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(keyTitleLabel);

    keyLabel.setText("--", juce::dontSendNotification);
    keyLabel.setFont(juce::Font("Arial", 18.0f, juce::Font::bold));
    addAndMakeVisible(keyLabel);

    detectedNotesTitleLabel.setText("DETECTED NOTES:", juce::dontSendNotification);
    detectedNotesTitleLabel.setFont(juce::Font("Arial", 14.0f, juce::Font::plain));
    addAndMakeVisible(detectedNotesTitleLabel);

    detectedNotesLabel.setText("--", juce::dontSendNotification);
    detectedNotesLabel.setFont(juce::Font("Arial", 18.0f, juce::Font::bold));
    addAndMakeVisible(detectedNotesLabel);

    contextLengthTitleLabel.setText("CONTEXT LENGTH:", juce::dontSendNotification);
    contextLengthTitleLabel.setFont(juce::Font("Arial", 14.0f, juce::Font::plain));
    addAndMakeVisible(contextLengthTitleLabel);

    secondsSlider.setSliderStyle(juce::Slider::IncDecButtons);
    secondsSlider.setRange(2.0, 10.0, 1.0);
//    secondsSlider.setValue(2.0);
    secondsSlider.setTextValueSuffix(" sec");
    secondsSlider.setTextBoxStyle(juce::Slider::TextEntryBoxPosition::TextBoxLeft, true, 50, 40);
    secondsSlider.setLookAndFeel(&sliderFontLookAndFeel);
    addAndMakeVisible(secondsSlider);
    secondsAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        audioProcessor.apvts, "ContextLength", secondsSlider);

    setSize(280, 280);

    startTimerHz(25);
}

PPDAudioProcessorEditor::~PPDAudioProcessorEditor()
{
    secondsSlider.setLookAndFeel(nullptr);
}

void PPDAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff340041));
}

void PPDAudioProcessorEditor::resized()
{
    keyTitleLabel.setBounds(15, 20, 240, 20);
    keyLabel.setBounds(15, 40, 240, 50);
    detectedNotesTitleLabel.setBounds(15, 105, 240, 20);
    detectedNotesLabel.setBounds(15, 125, 240, 50);
    contextLengthTitleLabel.setBounds(15, 190, 240, 20);
    secondsSlider.setBounds(20, 215, 80, 40);
}

void PPDAudioProcessorEditor::timerCallback()
{
    const auto notes = audioProcessor.getDetectedNotes();

    if (notes == lastDisplayedNotes)
        return;

    lastDisplayedNotes = notes;

    if (notes.empty())
    {
        detectedNotesLabel.setText("--", juce::dontSendNotification);
        keyLabel.setText("--", juce::dontSendNotification);
        return;
    }

    // Detected notes
    juce::String text;
    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (i > 0)
            text << "   ";
        text << midiNoteToName(notes[i]);
    }
    detectedNotesLabel.setText(text, juce::dontSendNotification);

    // Top-3 Spiral Array keys (name + score)
    const auto keys = audioProcessor.getEstimatedKeys();
    juce::String keyText;
    /*for (size_t i = 0; i < keys.size(); ++i)
    {
        if (i > 0)
            keyText << "   ";
        keyText << keys[i].first
            << " (" << juce::String(keys[i].second, 3) << ")";
    }*/
    for (size_t i = 0; i < keys.size(); ++i)
    {
        if (i > 0)
            keyText << "   ";
        keyText << (int)(i + 1) << ". " << keys[i].first;
    }
    keyLabel.setText(keyText, juce::dontSendNotification);
}

juce::String PPDAudioProcessorEditor::midiNoteToName(int midiNote)
{
    static const char* const names[12] =
    {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };

    const int octave = midiNote / 12 - 1;
    const int index = ((midiNote % 12) + 12) % 12; // defensive against negative input

    return juce::String(names[index]) + juce::String(octave);
}