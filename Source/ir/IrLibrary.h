#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "../Text.h"
#include <complex>
#include <functional>
#include <memory>
#include <vector>

namespace aurora
{
/** One impulse response, ready to compare and to convolve. */
struct IrInfo
{
    juce::File file;
    juce::String name;
    juce::AudioBuffer<float> samples;                 // mono, at IrLibrary::rate, loudness-normalised
    std::vector<std::complex<float>> response;        // FFT of 'samples' (bins 0 .. fftSize/2)
};

/**
    All IRs of a folder (and its subfolders). Every IR is converted to mono at 48 kHz, its leading
    silence is trimmed (so it lines up with the dry pickup signal when blended) and it is normalised
    to the same average gain between 200 Hz and 5 kHz, so the comparison is fair and the MIX knob
    blends signals of equal loudness.
*/
class IrLibrary
{
public:
    static constexpr double rate = 48000.0;
    static constexpr int fftOrder = 14;            // 16384-point responses
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int maxLengthSamples = 48000; // 1 s
    static constexpr int maxFiles = 3000;

    /** Loads one audio file as an IR. Returns nullptr (and the reason) on failure. */
    static std::shared_ptr<IrInfo> loadFile (const juce::File& file, juce::String& error);

    /** Builds an IR from samples (any rate, mono). */
    static std::shared_ptr<IrInfo> fromSamples (const float* data, int numSamples, double sampleRate,
                                                const juce::String& name, const juce::File& file);

    /** Scans a folder. 'progress' (done, total) may return false to cancel. Blocking: use a background thread. */
    void scan (const juce::File& folder, const std::function<bool (int, int)>& progress = {});

    void add (std::shared_ptr<IrInfo> ir) { irs.push_back (std::move (ir)); }

    int size() const noexcept { return (int) irs.size(); }
    std::shared_ptr<IrInfo> get (int i) const { return juce::isPositiveAndBelow (i, size()) ? irs[(size_t) i] : nullptr; }
    int indexOf (const juce::File& f) const;
    const juce::StringArray& getErrors() const noexcept { return errors; }

private:
    std::vector<std::shared_ptr<IrInfo>> irs;
    juce::StringArray errors;
};
}
