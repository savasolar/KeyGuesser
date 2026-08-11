#include "PluginProcessor.h"
#include "PluginEditor.h"

PPDAudioProcessorEditor::PPDAudioProcessorEditor(PPDAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{

    keyTitleLabel.setText("ESTIMATED KEY:", juce::dontSendNotification);
    keyTitleLabel.setFont(juce::Font("Arial", 14.0f, juce::Font::plain));
    keyTitleLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(keyTitleLabel);

    keyLabel.setText("C major", juce::dontSendNotification);
    keyLabel.setFont(juce::Font("Arial", 32.0f, juce::Font::bold));
    addAndMakeVisible(keyLabel);

    detectedNotesTitleLabel.setText("DETECTED NOTES:", juce::dontSendNotification);
    detectedNotesTitleLabel.setFont(juce::Font("Arial", 14.0f, juce::Font::plain));
    addAndMakeVisible(detectedNotesTitleLabel);

    detectedNotesLabel.setText("C10  C#10  D10", juce::dontSendNotification);
    detectedNotesLabel.setFont(juce::Font("Arial", 14.0f, juce::Font::bold));
    addAndMakeVisible(detectedNotesLabel);  // make it multiline

    contextLengthTitleLabel.setText("CONTEXT LENGTH:", juce::dontSendNotification);
    contextLengthTitleLabel.setFont(juce::Font("Arial", 14.0f, juce::Font::plain));
    addAndMakeVisible(contextLengthTitleLabel);

    secondsSlider.setSliderStyle(juce::Slider::IncDecButtons);
    secondsSlider.setRange(2.0, 10.0, 1.0);
    secondsSlider.setValue(2.0);
    secondsSlider.setTextValueSuffix(" sec");
    secondsSlider.setTextBoxStyle(juce::Slider::TextEntryBoxPosition::TextBoxLeft, true, 50, 40);
    secondsSlider.setLookAndFeel(&sliderFontLookAndFeel);
    addAndMakeVisible(secondsSlider);

    setSize(280, 280);
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