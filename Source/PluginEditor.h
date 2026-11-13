#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

namespace aurora
{
/** Colours and drawing of knobs / buttons. */
class AuroraLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AuroraLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
};

/** Pickup / target / result spectra. */
class SpectrumView : public juce::Component
{
public:
    void setCurves (bool hasInput, const BandArray& input, bool hasResult, const BandArray& result);
    void paint (juce::Graphics&) override;

private:
    bool hasInput = false, hasResult = false;
    BandArray input {}, result {};
};

/** One suggested IR: name, blend and how balanced the result is. */
class SuggestionCard : public juce::Button
{
public:
    SuggestionCard() : juce::Button ({}) {}
    void set (int rank, const juce::String& name, float mix, float affinity, bool selected);
    void clear();
    void paintButton (juce::Graphics&, bool over, bool down) override;

private:
    int rank = 0;
    juce::String name;
    float mix = 0.0f, affinity = 0.0f;
    bool selected = false, empty = true;
};
}

class AuroraAudioProcessorEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AuroraAudioProcessorEditor (AuroraAudioProcessor&);
    ~AuroraAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refresh();
    void updateSpectrum();
    void chooseFolder();
    void chooseIrFile();

    AuroraAudioProcessor& proc;
    aurora::AuroraLookAndFeel lnf;

    // Library
    juce::Label folderLabel, libraryStatus;
    juce::TextButton chooseFolderButton { "ESCOLHER PASTA" }, downloadButton { "BAIXAR IRs" };

    // LEARN
    juce::TextButton learnPickupButton { aurora::pt ("LEARN  ·  CAPTADOR") }, learnMicButton { aurora::pt ("LEARN  ·  CAPTADOR + MICROFONE") };
    juce::TextButton finishButton { "CONCLUIR" }, cancelButton { "CANCELAR" };
    juce::Label learnHint;
    float learnProgress = 0.0f, meter = 0.0f;
    bool listening = false;

    // Results
    aurora::SpectrumView spectrum;
    std::array<aurora::SuggestionCard, 3> cards;
    juce::Label statusLabel, dryLabel;

    // Controls
    juce::Slider mixKnob, toneKnob, outputKnob;
    juce::Label mixLabel, toneLabel, outputLabel, irLabel;
    juce::ComboBox inputBox;
    juce::ToggleButton bypassButton { "BYPASS" };
    juce::TextButton loadIrButton { aurora::pt ("ABRIR IR…") }, clearIrButton { "SEM IR" };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SliderAttachment> mixAttach, toneAttach, outputAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> inputAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttach;

    std::unique_ptr<juce::FileChooser> chooser;
    int lastChange = -1;
    float lastMix = -1.0f, lastTone = -100.0f;

    juce::Rectangle<int> libraryArea, learnArea, resultArea, controlsArea, progressArea, meterArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuroraAudioProcessorEditor)
};
