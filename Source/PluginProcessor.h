#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "ir/IrLibrary.h"
#include "ir/Matcher.h"
#include "ir/Learn.h"

/**
    Aurora IR Match: acoustic guitar pickup -> "microphone" sound.

    The pickup signal (channel 1, or L+R) is convolved with an IR from the user's library and
    blended with the dry pickup (MIX). LEARN listens to the guitar and either
      - picks the IR + blend from the library that best balances it (pickup only), or
      - computes this guitar's own IR from the pickup (channel 1) and a mic (channel 2).
*/
class AuroraAudioProcessor : public juce::AudioProcessor
{
public:
    AuroraAudioProcessor();
    ~AuroraAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState params;

    // ---- IR library -------------------------------------------------------------
    void setLibraryFolder (const juce::File& folder);
    juce::File getLibraryFolder() const;
    std::shared_ptr<const aurora::IrLibrary> getLibrary() const;
    bool isScanning() const noexcept         { return scanning.load(); }
    float getScanProgress() const noexcept   { return scanProgress.load(); }
    juce::String getLibraryStatus() const;

    // ---- LEARN ------------------------------------------------------------------
    void startLearn (aurora::Learn::Mode mode);
    void finishLearn()                        { learn.finish(); }
    void cancelLearn();
    const aurora::Learn& getLearn() const noexcept { return learn; }
    bool isBusy() const noexcept              { return learn.getState() == aurora::Learn::State::listening || matching.load(); }
    juce::String getStatus() const;

    struct Suggestion
    {
        juce::String name;
        juce::File file;
        float mix = 1.0f;
        float affinity = 0.0f;
        std::shared_ptr<aurora::IrInfo> ir; // may be null after a session reload (loaded from 'file')
    };
    std::vector<Suggestion> getSuggestions() const;
    float getDryAffinity() const;
    /** Learned pickup spectrum (false if nothing learned yet). */
    bool getLearnedSpectrum (aurora::BandArray& out) const;
    /** Loads suggestion i (its IR and its blend). */
    void applySuggestion (int index);

    // ---- current IR -------------------------------------------------------------
    bool loadIrFile (const juce::File& file, juce::String& error);
    void setIr (std::shared_ptr<aurora::IrInfo> ir);
    void clearIr();
    std::shared_ptr<aurora::IrInfo> getCurrentIr() const;

    /** Folder where the IRs made by "LEARN captador + microfone" are saved. */
    juce::File getOwnIrFolder() const;

    /** Bumped whenever the library, suggestions or current IR change (the editor refreshes). */
    std::atomic<int> changeCount { 0 };
    std::atomic<float> inputPeak { 0.0f };

    static constexpr double pickupLearnSeconds = 15.0;
    static constexpr double micLearnSeconds = 20.0;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void learnFinished();
    void runMatch (bool applyBest);
    void setMixParam (float mix01);
    void setStatus (const juce::String& s);
    void updateTone (float toneDb);

    aurora::Learn learn;
    juce::ThreadPool pool { 1 };

    mutable std::mutex lock; // guards everything below up to 'status'
    juce::File libraryFolder;
    std::shared_ptr<const aurora::IrLibrary> library = std::make_shared<aurora::IrLibrary>();
    std::shared_ptr<aurora::IrInfo> currentIr;
    std::vector<Suggestion> suggestions;
    aurora::BandArray learnedSpectrum {};
    bool hasSpectrum = false;
    float dryAffinity = 0.0f;
    juce::String status;
    int scanGeneration = 0;

    std::atomic<bool> scanning { false }, matching { false }, irActive { false };
    std::atomic<float> scanProgress { 0.0f };

    // ---- DSP --------------------------------------------------------------------
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* toneParam = nullptr;
    std::atomic<float>* outputParam = nullptr;
    std::atomic<float>* inputParam = nullptr;
    std::atomic<float>* bypassParam = nullptr;

    juce::dsp::Convolution convolution;
    juce::dsp::IIR::Filter<float> lowShelf, highShelf;
    float currentTone = -100.0f;
    double currentRate = 48000.0;
    juce::AudioBuffer<float> mono, wet;
    juce::SmoothedValue<float> mixSmooth, gainSmooth;

    JUCE_DECLARE_WEAK_REFERENCEABLE (AuroraAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AuroraAudioProcessor)
};
