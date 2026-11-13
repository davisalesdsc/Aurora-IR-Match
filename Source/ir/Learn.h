#pragma once

#include "Matcher.h"
#include <atomic>
#include <functional>
#include <mutex>

namespace aurora
{
/**
    LEARN: listens to the guitar while it is played.

      Mode::pickup        -> long-term spectrum of the pickup (channel 1), used to pick an IR
      Mode::pickupAndMic  -> also the cross-spectrum pickup -> microphone (channel 2), used to
                             compute this guitar's own IR (like a "match" IR)

    The audio thread only copies samples into a lock-free FIFO; all the analysis happens on the
    LEARN thread. Only moments where the guitar is actually played count towards the duration.
*/
class Learn : private juce::Thread
{
public:
    enum class Mode { pickup, pickupAndMic };
    enum class State { idle, listening, finished, failed };

    /** Analysis frame: 8192 points at 44.1/48 kHz (~0.17 s), doubled at 88.2/96 kHz and again above. */
    static int fftOrderFor (double sampleRate) noexcept { return 13 + (sampleRate > 50000.0 ? 1 : 0) + (sampleRate > 100000.0 ? 1 : 0); }
    int getFftSize() const noexcept { return fftSize; }

    Learn();
    ~Learn() override;

    void prepare (double sampleRate);

    /** Starts listening; finishes by itself after 'seconds' of playing (or call finish()). */
    void start (Mode mode, double seconds);
    /** Ends early with what was heard (needs at least a few seconds of playing). */
    void finish();
    void cancel();

    /** Audio thread. mic may be nullptr. */
    void push (const float* pickup, const float* mic, int numSamples) noexcept;

    State getState() const noexcept     { return state.load(); }
    Mode getMode() const noexcept       { return mode.load(); }
    float getProgress() const noexcept  { return progress.load(); }
    juce::String getMessage() const;

    /** Results (valid once finished). */
    BandArray getPickupSpectrum() const;
    /** This guitar's IR (pickupAndMic mode), at the analysis sample rate, one frame long. */
    std::vector<float> getLearnedIr() const;
    double getSampleRate() const noexcept { return sampleRate; }

    /** Called on the LEARN thread when listening ends (successfully or not). */
    std::function<void()> onFinished;

    /** Pure function (also used by the tests): IR from accumulated spectra. */
    static std::vector<float> irFromSpectra (const std::vector<std::complex<double>>& cross,
                                             const std::vector<double>& pickupPower,
                                             double sampleRate);

private:
    void run() override;
    void analyseFrame();
    void complete();
    void fail (const juce::String& why);

    double sampleRate = 48000.0;
    juce::AbstractFifo fifo { 1 << 18 };
    std::vector<float> fifoPickup, fifoMic;

    std::atomic<State> state { State::idle };
    std::atomic<Mode> mode { Mode::pickup };
    std::atomic<float> progress { 0.0f };
    std::atomic<bool> finishRequested { false }, hasMic { false };
    double targetSeconds = 15.0, activeSeconds = 0.0;

    int fftSize = 8192;
    std::vector<float> framePickup, frameMic, window;
    int frameFill = 0;
    std::vector<double> pickupPower, micPower;
    std::vector<std::complex<double>> cross;
    std::unique_ptr<juce::dsp::FFT> fft;

    mutable std::mutex resultLock;
    juce::String message;
    BandArray pickupSpectrum {};
    std::vector<float> learnedIr;
};
}
