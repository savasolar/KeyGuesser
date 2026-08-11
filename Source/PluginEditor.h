#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class PPDAudioProcessorEditor  : public juce::AudioProcessorEditor, public juce::Button::Listener
{
public:
    PPDAudioProcessorEditor (PPDAudioProcessor&);
    ~PPDAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void buttonClicked(juce::Button* button) override;

private:

    PPDAudioProcessor& audioProcessor;
    juce::TextButton testButton{ "Test" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PPDAudioProcessorEditor)
};
