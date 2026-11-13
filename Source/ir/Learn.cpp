#include "Learn.h"

namespace aurora
{
namespace
{
    constexpr float gateDb = -55.0f;     // frames quieter than this don't count (not playing)
    constexpr double minSeconds = 4.0;   // minimum playing time for a usable result
}

Learn::Learn() : juce::Thread ("Aurora LEARN")
{
    fifoPickup.resize ((size_t) fifo.getTotalSize());
    fifoMic.resize ((size_t) fifo.getTotalSize());
    prepare (48000.0);
}

Learn::~Learn()
{
    cancel();
}

void Learn::prepare (double sr)
{
    if (state.load() == State::listening)
        cancel();
    sampleRate = sr;
    const int order = fftOrderFor (sr);
    if (fft == nullptr || fft->getSize() != (1 << order))
    {
        fft = std::make_unique<juce::dsp::FFT> (order);
        fftSize = 1 << order;
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
    }
}

juce::String Learn::getMessage() const
{
    std::lock_guard<std::mutex> l (resultLock);
    return message;
}

BandArray Learn::getPickupSpectrum() const
{
    std::lock_guard<std::mutex> l (resultLock);
    return pickupSpectrum;
}

std::vector<float> Learn::getLearnedIr() const
{
    std::lock_guard<std::mutex> l (resultLock);
    return learnedIr;
}

void Learn::start (Mode m, double seconds)
{
    cancel();
    mode = m;
    targetSeconds = seconds;
    activeSeconds = 0.0;
    progress = 0.0f;
    frameFill = 0;
    finishRequested = false;
    hasMic = false;
    framePickup.assign ((size_t) fftSize, 0.0f);
    frameMic.assign ((size_t) fftSize, 0.0f);
    pickupPower.assign ((size_t) fftSize / 2 + 1, 0.0);
    micPower.assign ((size_t) fftSize / 2 + 1, 0.0);
    cross.assign ((size_t) fftSize / 2 + 1, {});
    fifo.reset();
    {
        std::lock_guard<std::mutex> l (resultLock);
        message = m == Mode::pickup ? pt ("Toque o violão (dedilhado e acordes)…") : pt ("Toque o violão (captador + microfone)…");
        learnedIr.clear();
    }
    state = State::listening;
    startThread();
}

void Learn::finish()
{
    finishRequested = true;
}

void Learn::cancel()
{
    signalThreadShouldExit();
    stopThread (3000);
    if (state.load() == State::listening)
        state = State::idle;
}

void Learn::push (const float* pickup, const float* mic, int n) noexcept
{
    if (state.load (std::memory_order_relaxed) != State::listening)
        return;
    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);
    std::copy (pickup, pickup + n1, fifoPickup.begin() + s1);
    std::copy (pickup + n1, pickup + n1 + n2, fifoPickup.begin() + s2);
    if (mic != nullptr)
    {
        std::copy (mic, mic + n1, fifoMic.begin() + s1);
        std::copy (mic + n1, mic + n1 + n2, fifoMic.begin() + s2);
        hasMic.store (true, std::memory_order_relaxed);
    }
    else
    {
        std::fill (fifoMic.begin() + s1, fifoMic.begin() + s1 + n1, 0.0f);
        std::fill (fifoMic.begin() + s2, fifoMic.begin() + s2 + n2, 0.0f);
    }
    fifo.finishedWrite (n1 + n2);
}

void Learn::run()
{
    const int hop = fftSize / 2;
    while (! threadShouldExit())
    {
        const int ready = fifo.getNumReady();
        if (ready == 0)
        {
            if (finishRequested.load())
            {
                complete();
                return;
            }
            wait (20);
            continue;
        }

        int s1, n1, s2, n2;
        const int want = juce::jmin (ready, fftSize - frameFill);
        fifo.prepareToRead (want, s1, n1, s2, n2);
        std::copy (fifoPickup.begin() + s1, fifoPickup.begin() + s1 + n1, framePickup.begin() + frameFill);
        std::copy (fifoMic.begin() + s1, fifoMic.begin() + s1 + n1, frameMic.begin() + frameFill);
        std::copy (fifoPickup.begin() + s2, fifoPickup.begin() + s2 + n2, framePickup.begin() + frameFill + n1);
        std::copy (fifoMic.begin() + s2, fifoMic.begin() + s2 + n2, frameMic.begin() + frameFill + n1);
        fifo.finishedRead (n1 + n2);
        frameFill += n1 + n2;

        if (frameFill == fftSize)
        {
            analyseFrame();
            // 50 % overlap
            std::copy (framePickup.begin() + hop, framePickup.end(), framePickup.begin());
            std::copy (frameMic.begin() + hop, frameMic.end(), frameMic.begin());
            frameFill = fftSize - hop;

            progress = (float) juce::jmin (1.0, activeSeconds / targetSeconds);
            if (activeSeconds >= targetSeconds || finishRequested.load())
            {
                complete();
                return;
            }
        }
    }
}

void Learn::analyseFrame()
{
    // Is the guitar being played?
    double e = 0.0;
    for (float v : framePickup) e += (double) v * v;
    const float rmsDb = (float) (10.0 * std::log10 (e / fftSize + 1.0e-20));
    if (rmsDb < gateDb)
        return;

    const int half = fftSize / 2;
    std::vector<std::complex<float>> a ((size_t) fftSize), b ((size_t) fftSize), A ((size_t) fftSize), B ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
    {
        a[(size_t) i] = framePickup[(size_t) i] * window[(size_t) i];
        b[(size_t) i] = frameMic[(size_t) i] * window[(size_t) i];
    }
    fft->perform (a.data(), A.data(), false);
    const bool withMic = mode.load() == Mode::pickupAndMic;
    if (withMic)
        fft->perform (b.data(), B.data(), false);

    for (int k = 0; k <= half; ++k)
    {
        const auto P = std::complex<double> (A[(size_t) k]);
        pickupPower[(size_t) k] += std::norm (P);
        if (withMic)
        {
            const auto M = std::complex<double> (B[(size_t) k]);
            micPower[(size_t) k] += std::norm (M);
            cross[(size_t) k] += M * std::conj (P);
        }
    }
    activeSeconds += (double) (fftSize / 2) / sampleRate;
}

void Learn::fail (const juce::String& why)
{
    {
        std::lock_guard<std::mutex> l (resultLock);
        message = why;
    }
    state = State::failed;
    if (onFinished)
        onFinished();
}

void Learn::complete()
{
    if (activeSeconds < juce::jmin (minSeconds, targetSeconds - 0.01))
    {
        fail ("Pouco som captado: toque pelo menos " + juce::String ((int) minSeconds) + " segundos (sinal no canal 1).");
        return;
    }

    const int half = fftSize / 2;
    std::vector<float> power ((size_t) half + 1);
    for (int k = 0; k <= half; ++k)
        power[(size_t) k] = (float) pickupPower[(size_t) k];
    const auto spectrum = Bands::fromPowerSpectrum (power.data(), fftSize, sampleRate);

    std::vector<float> ir;
    if (mode.load() == Mode::pickupAndMic)
    {
        double micTotal = 0.0, pickTotal = 0.0;
        for (int k = 1; k <= half; ++k) { micTotal += micPower[(size_t) k]; pickTotal += pickupPower[(size_t) k]; }
        if (! hasMic.load() || micTotal < pickTotal * 1.0e-6)
        {
            fail (pt ("Sem sinal do microfone no canal 2. Use uma faixa estéreo: canal 1 = captador, canal 2 = microfone."));
            return;
        }
        ir = irFromSpectra (cross, pickupPower, sampleRate);
        if (ir.empty())
        {
            fail (pt ("Não foi possível calcular o IR (captador e microfone não combinam). Tente de novo."));
            return;
        }
    }

    {
        std::lock_guard<std::mutex> l (resultLock);
        pickupSpectrum = spectrum;
        learnedIr = std::move (ir);
        message = "Pronto!";
    }
    progress = 1.0f;
    state = State::finished;
    if (onFinished)
        onFinished();
}

std::vector<float> Learn::irFromSpectra (const std::vector<std::complex<double>>& crossSpec,
                                         const std::vector<double>& pickPower, double sr)
{
    const int half = (int) crossSpec.size() - 1;
    const int n = half * 2;
    if (half < 16 || (int) pickPower.size() != half + 1)
        return {};

    // Regularised (Wiener-style) estimate H = Sxy / (Sxx + eps): bins where the pickup has
    // almost no energy don't explode.
    double meanPower = 0.0;
    for (int k = 1; k < half; ++k) meanPower += pickPower[(size_t) k];
    meanPower /= (half - 1);
    if (meanPower <= 0.0)
        return {};
    const double eps = meanPower * 1.0e-3;

    std::vector<std::complex<float>> spec ((size_t) n), time ((size_t) n);
    const double binHz = sr / n;
    for (int k = 0; k <= half; ++k)
    {
        auto h = crossSpec[(size_t) k] / (pickPower[(size_t) k] + eps);
        const double f = k * binHz;
        // Below 40 Hz and above 18 kHz the estimate is unreliable on a guitar: fade it out
        double g = 1.0;
        if (f < 40.0) g = f / 40.0;
        if (f > 18000.0) g = juce::jmax (0.0, 1.0 - (f - 18000.0) / 2000.0);
        h *= g;
        spec[(size_t) k] = std::complex<float> (h);
        if (k > 0 && k < half)
            spec[(size_t) (n - k)] = std::conj (spec[(size_t) k]);
    }

    int order = 0;
    while ((1 << order) < n) ++order;
    juce::dsp::FFT inverse (order);
    inverse.perform (spec.data(), time.data(), true);

    // Rotate so the main peak sits a little after the start (keeps a short pre-ringing)
    int peak = 0;
    float peakVal = 0.0f;
    for (int i = 0; i < n; ++i)
        if (std::abs (time[(size_t) i].real()) > peakVal) { peakVal = std::abs (time[(size_t) i].real()); peak = i; }
    if (peakVal <= 0.0f)
        return {};
    const int preSamples = (int) (0.002 * sr);
    const int start = (peak - preSamples + n) % n;
    const int len = n;

    std::vector<float> ir ((size_t) len);
    for (int i = 0; i < len; ++i)
        ir[(size_t) i] = time[(size_t) ((start + i) % n)].real();

    // Fade in the pre-ringing part and fade out the last 30 %
    for (int i = 0; i < preSamples && i < len; ++i)
        ir[(size_t) i] *= (float) i / (float) preSamples;
    const int fade = (int) (len * 0.3);
    for (int i = 0; i < fade; ++i)
        ir[(size_t) (len - fade + i)] *= 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) i / (float) fade);
    return ir;
}
}
