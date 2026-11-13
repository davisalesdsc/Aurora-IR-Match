#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <vector>

namespace aurora
{
/**
    1/6-octave analysis bands from 50 Hz to 16 kHz, and the TARGET: the average spectrum of a
    well-recorded, microphone'd steel-string acoustic guitar (body in the low mids, a smooth fall
    above ~1 kHz, no piezo "quack" peak). LEARN picks the IR + blend whose result is closest to it.
*/
struct Bands
{
    static constexpr int count = 51; // 50 Hz * 2^(k/6), k = 0..50  -> 50 Hz .. ~16 kHz

    static float centre (int k) noexcept { return 50.0f * std::pow (2.0f, (float) k / 6.0f); }
    static float lowEdge (int k) noexcept  { return centre (k) * std::pow (2.0f, -1.0f / 12.0f); }
    static float highEdge (int k) noexcept { return centre (k) * std::pow (2.0f, 1.0f / 12.0f); }

    /** Target level (dB, arbitrary reference) at a frequency; 'tilt' in dB per octave around 1 kHz. */
    static float targetDb (float hz, float tilt = 0.0f) noexcept
    {
        // Anchor points (Hz, dB): typical long-term spectrum of a mic'd acoustic guitar
        static constexpr float pts[][2] = {
            { 40.0f, -22.0f }, { 63.0f, -12.0f }, { 80.0f, -6.0f }, { 100.0f, -2.5f }, { 125.0f, 0.0f },
            { 160.0f, 1.0f }, { 200.0f, 1.0f }, { 250.0f, 0.0f }, { 315.0f, -1.0f }, { 400.0f, -2.0f },
            { 500.0f, -3.0f }, { 630.0f, -4.0f }, { 800.0f, -5.0f }, { 1000.0f, -6.0f }, { 1250.0f, -7.0f },
            { 1600.0f, -8.5f }, { 2000.0f, -10.0f }, { 2500.0f, -11.5f }, { 3150.0f, -13.0f }, { 4000.0f, -14.5f },
            { 5000.0f, -16.0f }, { 6300.0f, -18.0f }, { 8000.0f, -20.0f }, { 10000.0f, -23.0f },
            { 12500.0f, -27.0f }, { 16000.0f, -32.0f }, { 20000.0f, -38.0f } };
        constexpr int n = (int) (sizeof (pts) / sizeof (pts[0]));

        float db = pts[n - 1][1];
        if (hz <= pts[0][0])
            db = pts[0][1];
        else
            for (int i = 0; i < n - 1; ++i)
                if (hz <= pts[i + 1][0])
                {
                    const float t = std::log (hz / pts[i][0]) / std::log (pts[i + 1][0] / pts[i][0]);
                    db = pts[i][1] + t * (pts[i + 1][1] - pts[i][1]);
                    break;
                }
        return db + tilt * std::log2 (hz / 1000.0f);
    }

    /** How much each band counts when comparing spectra (the guitar's useful range matters most). */
    static float weight (int k) noexcept
    {
        const float f = centre (k);
        if (f < 70.0f)    return 0.3f;
        if (f < 100.0f)   return 0.7f;
        if (f > 10000.0f) return 0.3f;
        if (f > 7000.0f)  return 0.7f;
        return 1.0f;
    }

    /** Power spectrum (|X|^2 per FFT bin, bins 0..fftSize/2) -> band levels in dB. */
    static std::array<float, count> fromPowerSpectrum (const float* power, int fftSize, double sampleRate)
    {
        std::array<float, count> out {};
        const int half = fftSize / 2;
        const double binHz = sampleRate / fftSize;
        for (int k = 0; k < count; ++k)
        {
            int b0 = (int) std::ceil (lowEdge (k) / binHz), b1 = (int) std::floor (highEdge (k) / binHz);
            b0 = juce::jlimit (1, half, b0);
            b1 = juce::jlimit (b0, half, b1);
            double sum = 0.0;
            for (int b = b0; b <= b1; ++b)
                sum += power[b];
            out[(size_t) k] = (float) (10.0 * std::log10 (sum / (b1 - b0 + 1) + 1.0e-20));
        }
        return out;
    }
};
}
