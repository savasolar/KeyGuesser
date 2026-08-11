#include "PluginProcessor.h"
#include "PluginEditor.h"

PPDAudioProcessorEditor::PPDAudioProcessorEditor (PPDAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    addAndMakeVisible(testButton);
    testButton.addListener(this);
    setSize (400, 300);
}

PPDAudioProcessorEditor::~PPDAudioProcessorEditor()
{
}

void PPDAudioProcessorEditor::paint (juce::Graphics& g)
{

}

void PPDAudioProcessorEditor::buttonClicked(juce::Button* button)
{
    if (button == &testButton)
        audioProcessor.runTestTranscription();
}

void PPDAudioProcessorEditor::resized()
{
    testButton.setBounds(getLocalBounds().withSizeKeepingCentre(120, 40));
}
