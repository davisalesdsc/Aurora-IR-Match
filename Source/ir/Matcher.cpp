#include "Matcher.h"
#include <algorithm>

namespace aurora
{
float Matcher::distance (const BandArray& resultDb, float tilt)
{
    // Weighted RMS of (result - target) after removing the mean (level does not matter)
    double sw = 0.0, sd = 0.0;
    BandArray d {};
    for (int k = 0; k < Bands::count; ++k)
    {
        d[(size_t) k] = resultDb[(size_t) k] - Bands::targetDb (Bands::centre (k), tilt);
        sw += Bands::weight (k);
        sd += Bands::weight (k) * d[(size_t) k];
    }
    const double mean = sd / sw;
    double acc = 0.0;
    for (int k = 0; k < Bands::count; ++k)
    {
        const double e = d[(size_t) k] - mean;
        acc += Bands::weight (k) * e * e;
    }
    return (float) std::sqrt (acc / sw);
}

MatchResult Matcher::evaluate (const BandArray& inputDb, const IrInfo& ir, float mix, float tilt)
{
    MatchResult r;
    r.mix = mix;
    const double binHz = IrLibrary::rate / IrLibrary::fftSize;
    const int half = IrLibrary::fftSize / 2;
    const std::complex<float> dry (1.0f - mix, 0.0f);

    for (int k = 0; k < Bands::count; ++k)
    {
        int b0 = (int) std::ceil (Bands::lowEdge (k) / binHz), b1 = (int) std::floor (Bands::highEdge (k) / binHz);
        b0 = juce::jlimit (1, half, b0);
        b1 = juce::jlimit (b0, half, b1);
        double p = 0.0;
        for (int b = b0; b <= b1; ++b)
            p += std::norm (dry + mix * ir.response[(size_t) b]);
        r.resultDb[(size_t) k] = inputDb[(size_t) k] + (float) (10.0 * std::log10 (p / (b1 - b0 + 1) + 1.0e-20));
    }
    r.distanceDb = distance (r.resultDb, tilt);
    r.affinity = affinityFromDistance (r.distanceDb);
    return r;
}

MatchResult Matcher::scoreDry (const BandArray& inputDb, float tilt)
{
    MatchResult r;
    r.mix = 0.0f;
    r.resultDb = inputDb;
    r.distanceDb = distance (inputDb, tilt);
    r.affinity = affinityFromDistance (r.distanceDb);
    return r;
}

std::vector<MatchResult> Matcher::match (const BandArray& inputDb, const IrLibrary& library, float tilt, int topN)
{
    std::vector<MatchResult> best;
    for (int i = 0; i < library.size(); ++i)
    {
        const auto ir = library.get (i);
        MatchResult bestForIr;
        for (int step = 4; step <= 20; ++step) // blend 20 % .. 100 % in 5 % steps
        {
            auto r = evaluate (inputDb, *ir, (float) step / 20.0f, tilt);
            if (r.distanceDb < bestForIr.distanceDb)
                bestForIr = r;
        }
        bestForIr.index = i;
        best.push_back (bestForIr);
    }
    std::sort (best.begin(), best.end(), [] (const auto& a, const auto& b) { return a.distanceDb < b.distanceDb; });
    if ((int) best.size() > topN)
        best.resize ((size_t) topN);
    return best;
}
}
