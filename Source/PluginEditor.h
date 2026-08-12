#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class PPDAudioProcessorEditor : public juce::AudioProcessorEditor, public juce::Timer
{
public:
    PPDAudioProcessorEditor(PPDAudioProcessor&);
    ~PPDAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;


private:

    // juce::Slider has no setFont() of its own - its text box is actually a
    // juce::Label created internally by the LookAndFeel. This LookAndFeel
    // override lets that internal label use the same Arial font as the rest
    // of the UI while leaving every other label's font untouched.
    // Also draws the IncDecButtons with transparent fill (but keeps border + hover).
    class SliderFontLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        juce::Font getLabelFont(juce::Label& label) override
        {
            if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
                return juce::Font("Arial", 14.0f, juce::Font::plain);

            return juce::LookAndFeel_V4::getLabelFont(label);
        }

        void drawButtonBackground(juce::Graphics& g, juce::Button& button,
            const juce::Colour& /*backgroundColour*/,
            bool shouldDrawButtonAsHighlighted,
            bool shouldDrawButtonAsDown) override
        {
            constexpr float cornerSize = 0.0f;
            auto bounds = button.getLocalBounds().toFloat().reduced(0.5f, 0.5f);

            // Transparent by default; only a light semi-transparent fill on hover/down
            juce::Colour fill = juce::Colours::transparentBlack;
            if (shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted)
                fill = juce::Colours::white.withAlpha(shouldDrawButtonAsDown ? 0.25f : 0.12f);

            g.setColour(fill);

            const bool flatOnLeft = button.isConnectedOnLeft();
            const bool flatOnRight = button.isConnectedOnRight();
            const bool flatOnTop = button.isConnectedOnTop();
            const bool flatOnBottom = button.isConnectedOnBottom();

            if (flatOnLeft || flatOnRight || flatOnTop || flatOnBottom)
            {
                juce::Path path;
                path.addRoundedRectangle(bounds.getX(), bounds.getY(),
                    bounds.getWidth(), bounds.getHeight(),
                    cornerSize, cornerSize,
                    !(flatOnLeft || flatOnTop),
                    !(flatOnRight || flatOnTop),
                    !(flatOnLeft || flatOnBottom),
                    !(flatOnRight || flatOnBottom));

                g.fillPath(path);

                g.setColour(button.findColour(juce::ComboBox::outlineColourId));
                g.strokePath(path, juce::PathStrokeType(1.0f));
            }
            else
            {
                g.fillRoundedRectangle(bounds, cornerSize);

                g.setColour(button.findColour(juce::ComboBox::outlineColourId));
                g.drawRoundedRectangle(bounds, cornerSize, 1.0f);
            }
        }
    };

    PPDAudioProcessor& audioProcessor;

    void timerCallback() override;

    // Converts a MIDI note number (0-127) to a "name + octave" string,
    // e.g. 61 -> "C#4". Middle C (60) is C4, matching standard MIDI convention.
    static juce::String midiNoteToName(int midiNote);

    // Last set of notes actually pushed into detectedNotesLabel, so the
    // timer callback only touches the label when something has changed.
    std::vector<int> lastDisplayedNotes;

    juce::Label keyTitleLabel;
    juce::Label keyLabel;
    juce::Label detectedNotesTitleLabel;
    juce::Label detectedNotesLabel;
    juce::Label contextLengthTitleLabel;

    juce::Slider secondsSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> secondsAttachment;

    SliderFontLookAndFeel sliderFontLookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PPDAudioProcessorEditor)
};