#pragma once

#include "Bands.h"
#include "IrLibrary.h"

namespace aurora
{
using BandArray = std::array<float, Bands::count>;

struct MatchResult
{
    int index = -1;          // IR in the library (-1: the dry pickup alone)
    float mix = 1.0f;        // 0 = only the pickup (line), 1 = only the IR
    float distanceDb = 99.0f;
    float affinity = 0.0f;   // 0..100 %, shown to the user
    BandArray resultDb {};   // predicted result spectrum
};

/**
    Finds the IRs (and blends) that bring the learned pickup spectrum closest to the target.

    For each IR and each blend m the result is predicted per band as
        input(f) + 10 log10 mean |(1 - m) + m H(f)|^2
    (complex sum: the comb filtering between the dry pickup and the IR counts), compared with the
    target after removing the overall level difference, weighted towards the guitar's range.
*/
class Matcher
{
public:
    static std::vector<MatchResult> match (const BandArray& inputDb, const IrLibrary& library,
                                           float targetTilt = 0.0f, int topN = 3);

    /** The dry pickup alone, scored the same way (to show how much an IR helps). */
    static MatchResult scoreDry (const BandArray& inputDb, float targetTilt = 0.0f);

    static MatchResult evaluate (const BandArray& inputDb, const IrInfo& ir, float mix, float targetTilt);

    static float distance (const BandArray& resultDb, float targetTilt);
    static float affinityFromDistance (float d) { return 100.0f * std::exp (-d / 6.0f); }
};
}
