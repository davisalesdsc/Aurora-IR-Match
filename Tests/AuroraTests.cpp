// Aurora IR Match tests.
//
//   AuroraTests                     -> unit + integration tests (exit code != 0 on failure)
//   AuroraTests --snapshot out.png  -> picture of the plugin window after a simulated LEARN

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <iostream>

using namespace aurora;

namespace
{
int failures = 0;

void check (bool okay, const juce::String& what)
{
    std::cout << (okay ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
    if (! okay)
        ++failures;
}

constexpr double fs = 48000.0;

/** A synthetic "pickup -> mic" IR: direct sound + body resonances + a brightness filter. */
std::vector<float> synthIr (int variant, int len = 2400)
{
    juce::Random rng (1000 + variant);
    std::vector<float> ir ((size_t) len, 0.0f);
    ir[0] = 1.0f;
    struct Res { float f, g, tau; };
    const Res res[] = {
        { 90.0f + 160.0f * rng.nextFloat(), 0.06f + 0.10f * rng.nextFloat(), 0.004f + 0.006f * rng.nextFloat() },
        { 380.0f + 500.0f * rng.nextFloat(), (rng.nextBool() ? 1.0f : -1.0f) * (0.04f + 0.08f * rng.nextFloat()), 0.003f + 0.004f * rng.nextFloat() },
        { 1800.0f + 2500.0f * rng.nextFloat(), (rng.nextBool() ? 1.0f : -1.0f) * (0.05f + 0.15f * rng.nextFloat()), 0.001f + 0.002f * rng.nextFloat() },
    };
    for (const auto& r : res)
        for (int n = 1; n < len; ++n)
            ir[(size_t) n] += r.g * std::exp (-(float) n / (r.tau * (float) fs)) * std::sin (juce::MathConstants<float>::twoPi * r.f * (float) n / (float) fs);

    // One-pole low-pass: each variant has a different brightness
    const float cutoff = 2500.0f + 1500.0f * (float) variant;
    const float a = std::exp (-juce::MathConstants<float>::twoPi * cutoff / (float) fs);
    float y = 0.0f;
    for (auto& v : ir) { y = (1.0f - a) * v + a * y; v = y; }
    return ir;
}

bool writeWav (const juce::File& f, const std::vector<std::vector<float>>& channels, double rate, int bits)
{
    f.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (f.createOutputStream());
    if (stream == nullptr)
        return false;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate)
                                                                              .withNumChannels ((int) channels.size())
                                                                              .withBitsPerSample (bits));
    if (writer == nullptr)
        return false;
    juce::AudioBuffer<float> buf ((int) channels.size(), (int) channels[0].size());
    for (int c = 0; c < (int) channels.size(); ++c)
        buf.copyFrom (c, 0, channels[(size_t) c].data(), (int) channels[(size_t) c].size());
    return writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

/** Band levels of an IR (10 log10 of the mean |H|^2 per band). */
BandArray irBands (const IrInfo& ir)
{
    BandArray zero {};
    return Matcher::evaluate (zero, ir, 1.0f, 0.0f).resultDb;
}

/** RMS difference between two band curves (100 Hz .. 10 kHz) after removing the mean. */
float shapeError (const BandArray& a, const BandArray& b)
{
    double sum = 0.0, sum2 = 0.0;
    int n = 0;
    for (int k = 0; k < Bands::count; ++k)
    {
        if (Bands::centre (k) < 100.0f || Bands::centre (k) > 10000.0f)
            continue;
        const double d = a[(size_t) k] - b[(size_t) k];
        sum += d; sum2 += d * d; ++n;
    }
    const double mean = sum / n;
    return (float) std::sqrt (juce::jmax (0.0, sum2 / n - mean * mean));
}

/** Stationary noise whose spectrum follows 'bandsDb' (interpolated between band centres). */
std::vector<float> shapedNoise (const BandArray& bandsDb, int order, int seed)
{
    const int n = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<std::complex<float>> spec ((size_t) n), time ((size_t) n);
    juce::Random rng (seed);
    const double binHz = fs / n;
    for (int k = 1; k < n / 2; ++k)
    {
        const float f = (float) (k * binHz);
        float db;
        const float pos = 6.0f * std::log2 (f / 50.0f); // band index (fractional)
        if (pos <= 0.0f) db = bandsDb[0] - 12.0f * (-pos / 6.0f);
        else if (pos >= Bands::count - 1) db = bandsDb[Bands::count - 1] - 12.0f * ((pos - (Bands::count - 1)) / 6.0f);
        else
        {
            const int i = (int) pos;
            const float t = pos - (float) i;
            db = bandsDb[(size_t) i] * (1.0f - t) + bandsDb[(size_t) i + 1] * t;
        }
        const float mag = juce::Decibels::decibelsToGain (db);
        const float ph = rng.nextFloat() * juce::MathConstants<float>::twoPi;
        spec[(size_t) k] = std::polar (mag, ph);
        spec[(size_t) (n - k)] = std::conj (spec[(size_t) k]);
    }
    fft.perform (spec.data(), time.data(), true);
    std::vector<float> out ((size_t) n);
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) { out[(size_t) i] = time[(size_t) i].real(); peak = juce::jmax (peak, std::abs (out[(size_t) i])); }
    for (auto& v : out) v *= 0.3f / peak;
    return out;
}

std::vector<float> whiteNoise (int len, int seed, float amp = 0.1f)
{
    juce::Random rng (seed);
    std::vector<float> v ((size_t) len);
    for (auto& s : v) s = (rng.nextFloat() * 2.0f - 1.0f) * amp;
    return v;
}

std::vector<float> convolveDirect (const std::vector<float>& x, const std::vector<float>& h)
{
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const size_t m = std::min (h.size(), i + 1);
        float acc = 0.0f;
        for (size_t j = 0; j < m; ++j)
            acc += h[j] * x[i - j];
        y[i] = acc;
    }
    return y;
}

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

template <typename Fn>
bool waitFor (Fn condition, int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        if (condition())
            return true;
        pump (10);
    }
    return condition();
}

/** Feeds audio through the processor in 512-sample blocks (L = a, R = b or a). */
void feed (AuroraAudioProcessor& p, const std::vector<float>& a, const std::vector<float>* b, int start, int count)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    for (int pos = start; pos < start + count; pos += 512)
    {
        const int n = juce::jmin (512, start + count - pos);
        buf.setSize (2, n, false, false, true);
        for (int i = 0; i < n; ++i)
        {
            buf.setSample (0, i, a[(size_t) (pos + i) % a.size()]);
            buf.setSample (1, i, (b != nullptr ? *b : a)[(size_t) (pos + i) % a.size()]);
        }
        p.processBlock (buf, midi);
        if (((pos - start) / 512) % 8 == 7)
            juce::Thread::sleep (1); // let the LEARN thread keep up (it runs faster than real time anyway)
    }
}

/** A folder with 'count' synthetic IRs named ir0..irN, written in a mix of formats. */
juce::File makeLibrary (const juce::String& name, int count)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    dir.deleteRecursively();
    dir.getChildFile ("sub").createDirectory();
    for (int v = 0; v < count; ++v)
    {
        const auto ir = synthIr (v);
        if (v % 3 == 0)
            writeWav (dir.getChildFile ("ir" + juce::String (v) + ".wav"), { ir }, fs, 24);
        else if (v % 3 == 1) // stereo 44.1 kHz (resampled on load)
        {
            std::vector<float> rs ((size_t) (ir.size() * 44100 / 48000) + 1);
            juce::WindowedSincInterpolator interp;
            std::vector<float> padded (ir); padded.resize (ir.size() + 64, 0.0f);
            interp.process (48000.0 / 44100.0, padded.data(), rs.data(), (int) rs.size());
            writeWav (dir.getChildFile ("sub/ir" + juce::String (v) + ".wav"), { rs, rs }, 44100.0, 16);
        }
        else
            writeWav (dir.getChildFile ("ir" + juce::String (v) + ".wav"), { ir }, fs, 32);
    }
    dir.getChildFile ("leia-me.txt").replaceWithText ("not audio");
    dir.getChildFile ("quebrado.wav").replaceWithText ("this is not a wav file");
    return dir;
}

//==============================================================================
void bandTests()
{
    std::cout << "Bands / target" << std::endl;
    check (std::abs (Bands::centre (0) - 50.0f) < 0.01f && Bands::centre (Bands::count - 1) > 15000.0f, "51 bands from 50 Hz to ~16 kHz");
    check (Bands::targetDb (160.0f) > Bands::targetDb (1000.0f) && Bands::targetDb (1000.0f) > Bands::targetDb (8000.0f),
           "target: body > mids > highs");
    check (std::abs (Bands::targetDb (2000.0f, 2.0f) - Bands::targetDb (2000.0f) - 2.0f) < 0.01f, "tilt adds dB per octave");

    std::vector<float> flat (4097, 1.0f);
    const auto b = Bands::fromPowerSpectrum (flat.data(), 8192, fs);
    bool allZero = true;
    for (float v : b) allZero = allZero && std::abs (v) < 0.01f;
    check (allZero, "flat power spectrum -> 0 dB in every band");
}

void libraryTests()
{
    std::cout << "IR library" << std::endl;
    const auto dir = makeLibrary ("aurora-test-lib", 6);
    IrLibrary lib;
    int lastDone = -1, lastTotal = -1;
    lib.scan (dir, [&] (int d, int t) { lastDone = d; lastTotal = t; return true; });
    check (lib.size() == 6, "scans subfolders, 16/24/32-bit, mono/stereo, 44.1/48 kHz: " + juce::String (lib.size()) + " IRs");
    check (lib.getErrors().size() == 1 && lib.getErrors()[0].contains ("quebrado"), "broken .wav reported, .txt ignored");
    check (lastDone == lastTotal && lastTotal == 7, "progress reaches the end");

    // Resampled IR matches the original
    const auto a = lib.get (lib.indexOf (dir.getChildFile ("ir0.wav")));
    const auto b = lib.get (lib.indexOf (dir.getChildFile ("sub/ir1.wav")));
    juce::String err;
    auto ref1 = IrLibrary::fromSamples (synthIr (1).data(), 2400, fs, "ref", {});
    check (a != nullptr && b != nullptr && ref1 != nullptr, "IRs found by file");
    if (b != nullptr && ref1 != nullptr)
    {
        const float e = shapeError (irBands (*b), irBands (*ref1));
        check (e < 0.3f, "44.1 kHz stereo IR resampled correctly (shape error " + juce::String (e, 2) + " dB)");
    }

    // Normalisation: equal mid-band loudness
    if (a != nullptr && b != nullptr)
    {
        auto mid = [] (const IrInfo& ir) {
            const auto bands = irBands (ir);
            double s = 0.0; int n = 0;
            for (int k = 0; k < Bands::count; ++k)
                if (Bands::centre (k) > 250.0f && Bands::centre (k) < 4000.0f) { s += juce::Decibels::decibelsToGain (bands[(size_t) k] * 0.5); ++n; }
            return juce::Decibels::gainToDecibels (s / n);
        };
        check (std::abs (mid (*a) - mid (*b)) < 1.5, "IRs normalised to similar loudness");
    }

    // Leading silence trimmed
    std::vector<float> delayed (3000, 0.0f);
    delayed[500] = 1.0f; delayed[520] = 0.3f;
    auto t = IrLibrary::fromSamples (delayed.data(), (int) delayed.size(), fs, "d", {});
    check (t != nullptr && std::abs (t->samples.getSample (0, 8)) > 0.5f, "leading silence trimmed (onset kept at sample 8)");
}

void matcherTests()
{
    std::cout << "Matcher" << std::endl;
    IrLibrary lib;
    for (int v = 0; v < 8; ++v)
    {
        const auto ir = synthIr (v);
        lib.add (IrLibrary::fromSamples (ir.data(), (int) ir.size(), fs, "ir" + juce::String (v), {}));
    }

    for (int truth : { 2, 5 })
    {
        // A pickup that becomes exactly the target through IR 'truth'
        const auto h = irBands (*lib.get (truth));
        BandArray input {};
        for (int k = 0; k < Bands::count; ++k)
            input[(size_t) k] = Bands::targetDb (Bands::centre (k)) - h[(size_t) k] + 7.0f;

        const auto res = Matcher::match (input, lib);
        check (res.size() == 3, "returns the top 3");
        check (! res.empty() && res[0].index == truth, "picks the right IR (ir" + juce::String (truth) + "), got ir"
                                                           + juce::String (res.empty() ? -1 : res[0].index));
        check (! res.empty() && res[0].mix >= 0.9f && res[0].affinity > 95.0f,
               "with blend " + juce::String (res.empty() ? 0.0f : res[0].mix * 100.0f, 0) + "% and affinity "
                   + juce::String (res.empty() ? 0.0f : res[0].affinity, 1) + "%");
        check (res.size() == 3 && res[1].distanceDb >= res[0].distanceDb && res[2].distanceDb >= res[1].distanceDb, "sorted by distance");
        check (Matcher::scoreDry (input).affinity < res[0].affinity, "better than the pickup alone");
    }

    // A pickup that is already balanced: blending less IR should win over 100 % of a coloured IR
    BandArray balanced {};
    for (int k = 0; k < Bands::count; ++k)
        balanced[(size_t) k] = Bands::targetDb (Bands::centre (k));
    const auto res = Matcher::match (balanced, lib);
    check (! res.empty() && res[0].mix < 1.0f, "already-balanced pickup -> partial blend (" + juce::String (res.empty() ? 0.0f : res[0].mix * 100.0f, 0) + "% IR)");
}

Learn::State waitLearn (const Learn& l, int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (l.getState() == Learn::State::listening && juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (5);
    return l.getState();
}

void pushAll (Learn& l, const std::vector<float>& a, const std::vector<float>* b, int len)
{
    for (int pos = 0; pos + 512 <= len; pos += 512)
    {
        l.push (a.data() + pos, b != nullptr ? b->data() + pos : nullptr, 512);
        if ((pos / 512) % 8 == 7)
            juce::Thread::sleep (1);
    }
}

void learnTests()
{
    std::cout << "LEARN" << std::endl;
    Learn learn;
    learn.prepare (fs);
    std::atomic<int> finished { 0 };
    learn.onFinished = [&] { ++finished; };

    // Pickup: white noise -> flat spectrum
    const auto noise = whiteNoise ((int) (fs * 8), 7);
    learn.start (Learn::Mode::pickup, 30.0);
    pushAll (learn, noise, nullptr, (int) noise.size());
    learn.finish();
    check (waitLearn (learn, 5000) == Learn::State::finished, "pickup LEARN finishes early on request");
    check (finished.load() == 1, "onFinished called once");
    {
        const auto s = learn.getPickupSpectrum();
        float lo = 1000.0f, hi = -1000.0f;
        for (int k = 0; k < Bands::count; ++k)
            if (Bands::centre (k) >= 100.0f) { lo = juce::jmin (lo, s[(size_t) k]); hi = juce::jmax (hi, s[(size_t) k]); }
        check (hi - lo < 2.0f, "white noise measured flat (spread " + juce::String (hi - lo, 2) + " dB)");
    }

    // Auto-finish after the requested duration
    learn.start (Learn::Mode::pickup, 3.0);
    learn.push (noise.data(), nullptr, 0);
    pushAll (learn, noise, nullptr, (int) (fs * 5));
    {
        const auto st = waitLearn (learn, 5000);
        check (st == Learn::State::finished && learn.getProgress() >= 1.0f,
               "finishes by itself after the set time (state " + juce::String ((int) st) + ", progress " + juce::String (learn.getProgress(), 2) + ")");
    }

    // Silence only counts as "not playing"
    std::vector<float> silence ((size_t) fs * 6, 0.0f);
    learn.start (Learn::Mode::pickup, 15.0);
    pushAll (learn, silence, nullptr, (int) silence.size());
    learn.finish();
    check (waitLearn (learn, 5000) == Learn::State::failed && learn.getMessage().contains ("Pouco som"),
           "silence -> clear message: " + learn.getMessage());

    // Mic mode without a mic signal
    learn.start (Learn::Mode::pickupAndMic, 20.0);
    pushAll (learn, noise, nullptr, (int) (fs * 6));
    learn.finish();
    check (waitLearn (learn, 5000) == Learn::State::failed && learn.getMessage().contains ("canal 2"),
           "mic LEARN without channel 2 -> " + learn.getMessage());

    // Pickup + mic: recovers the IR between them
    const auto truthIr = synthIr (3, 1500);
    std::vector<float> delayedIr (30, 0.0f);
    delayedIr.insert (delayedIr.end(), truthIr.begin(), truthIr.end()); // ~0.6 ms acoustic delay
    const auto pickup = whiteNoise ((int) (fs * 10), 11, 0.2f);
    auto mic = convolveDirect (pickup, delayedIr);
    const auto hiss = whiteNoise ((int) mic.size(), 12, 0.002f); // mic self-noise
    for (size_t i = 0; i < mic.size(); ++i) mic[i] += hiss[i];

    learn.start (Learn::Mode::pickupAndMic, 8.0);
    pushAll (learn, pickup, &mic, (int) pickup.size());
    check (waitLearn (learn, 8000) == Learn::State::finished, "pickup + mic LEARN finishes");
    const auto learned = learn.getLearnedIr();
    check (learned.size() == (size_t) learn.getFftSize(), "IR length " + juce::String ((double) learned.size() / fs, 2) + " s");
    auto got = IrLibrary::fromSamples (learned.data(), (int) learned.size(), fs, "learned", {});
    auto ref = IrLibrary::fromSamples (truthIr.data(), (int) truthIr.size(), fs, "truth", {});
    check (got != nullptr && ref != nullptr, "learned IR is usable");
    if (got != nullptr && ref != nullptr)
    {
        const float e = shapeError (irBands (*got), irBands (*ref));
        check (e < 1.0f, "learned IR matches the real one (error " + juce::String (e, 2) + " dB, 100 Hz - 10 kHz)");
    }
    learn.onFinished = nullptr;
}

//==============================================================================
float maxDiff (const juce::AudioBuffer<float>& a, const std::vector<float>& expected, int ch)
{
    float d = 0.0f;
    for (int i = 0; i < (int) expected.size(); ++i)
        d = juce::jmax (d, std::abs (a.getSample (ch, i) - expected[(size_t) i]));
    return d;
}

void processBlocks (AuroraAudioProcessor& p, int blocks)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        p.processBlock (buf, midi);
        juce::Thread::sleep (2);
    }
}

void setParam (AuroraAudioProcessor& p, const juce::String& id, float value)
{
    auto* param = p.params.getParameter (id);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

void processorTests()
{
    std::cout << "Processor" << std::endl;
    AuroraAudioProcessor p;
    p.setPlayConfigDetails (2, 2, fs, 512);
    p.prepareToPlay (fs, 512);
    juce::MidiBuffer midi;

    // No IR: (L + R) / 2 on both outputs
    {
        const auto l = whiteNoise (512, 1), r = whiteNoise (512, 2);
        juce::AudioBuffer<float> buf (2, 512);
        buf.copyFrom (0, 0, l.data(), 512);
        buf.copyFrom (1, 0, r.data(), 512);
        processBlocks (p, 30); // let the mix smoothing settle
        p.processBlock (buf, midi);
        std::vector<float> expected (512);
        for (int i = 0; i < 512; ++i) expected[(size_t) i] = 0.5f * (l[(size_t) i] + r[(size_t) i]);
        check (maxDiff (buf, expected, 0) < 1.0e-6f && maxDiff (buf, expected, 1) < 1.0e-6f, "no IR: pickup passes through (L+R mono on both sides)");

        setParam (p, "input", 1.0f);
        buf.copyFrom (0, 0, l.data(), 512);
        buf.copyFrom (1, 0, r.data(), 512);
        p.processBlock (buf, midi);
        check (maxDiff (buf, l, 0) < 1.0e-6f && maxDiff (buf, l, 1) < 1.0e-6f, pt ("input 'só canal 1': only L is used"));
        setParam (p, "input", 0.0f);
    }

    // Convolution: impulse in -> the (normalised) IR out
    std::vector<float> irData (11, 0.0f);
    irData[0] = 1.0f; irData[10] = 0.5f;
    auto ir = IrLibrary::fromSamples (irData.data(), (int) irData.size(), fs, "teste", {});
    p.setIr (ir);
    processBlocks (p, 120); // background load + crossfade of juce::dsp::Convolution
    std::vector<float> expected (32, 0.0f);
    for (int i = 0; i < ir->samples.getNumSamples(); ++i) expected[(size_t) i] = ir->samples.getSample (0, i);
    juce::AudioBuffer<float> buf (2, 512);
    buf.clear();
    buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f);
    p.processBlock (buf, midi);
    check (maxDiff (buf, expected, 0) < 1.0e-4f && maxDiff (buf, expected, 1) < 1.0e-4f,
           "MIX 100%: output = pickup convolved with the IR, zero latency (err " + juce::String (maxDiff (buf, expected, 0), 6) + ")");
    check (p.getLatencySamples() == 0, "reports zero latency");

    setParam (p, "mix", 50.0f);
    processBlocks (p, 20);
    buf.clear();
    buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f);
    p.processBlock (buf, midi);
    std::vector<float> half (expected);
    for (auto& v : half) v *= 0.5f;
    half[0] += 0.5f;
    check (maxDiff (buf, half, 0) < 1.0e-4f, "MIX 50%: half pickup + half IR");

    setParam (p, "output", -6.0f);
    processBlocks (p, 20);
    buf.clear();
    buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f);
    p.processBlock (buf, midi);
    check (std::abs (buf.getSample (0, 0) - half[0] * juce::Decibels::decibelsToGain (-6.0f)) < 1.0e-3f, "output gain -6 dB");
    setParam (p, "output", 0.0f);

    // Tone: brighter = more highs relative to lows
    {
        setParam (p, "mix", 0.0f);
        setParam (p, "tone", 6.0f);
        auto measure = [&] (float hz) {
            juce::AudioBuffer<float> b (2, 512);
            double phase = 0.0, e = 0.0;
            for (int blk = 0; blk < 40; ++blk)
            {
                for (int i = 0; i < 512; ++i) { const float s = 0.1f * (float) std::sin (phase); phase += juce::MathConstants<double>::twoPi * hz / fs; b.setSample (0, i, s); b.setSample (1, i, s); }
                p.processBlock (b, midi);
                if (blk >= 20) for (int i = 0; i < 512; ++i) e += b.getSample (0, i) * b.getSample (0, i);
            }
            return juce::Decibels::gainToDecibels (std::sqrt (e / (20 * 512)) / (0.1 / std::sqrt (2.0)));
        };
        const auto low = measure (100.0f), high = measure (8000.0f);
        check (high - low > 4.5 && high - low < 7.5, "TOM +6: highs " + juce::String (high - low, 1) + " dB above lows");
        setParam (p, "tone", 0.0f);
        setParam (p, "mix", 100.0f);
    }

    // Bypass
    setParam (p, "bypass", 1.0f);
    {
        const auto l = whiteNoise (512, 3), r = whiteNoise (512, 4);
        buf.copyFrom (0, 0, l.data(), 512);
        buf.copyFrom (1, 0, r.data(), 512);
        p.processBlock (buf, midi);
        check (maxDiff (buf, l, 0) < 1.0e-7f && maxDiff (buf, r, 1) < 1.0e-7f, "bypass: untouched");
    }
    setParam (p, "bypass", 0.0f);

    // Buses
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (juce::AudioChannelSet::mono()); l.outputBuses.add (juce::AudioChannelSet::stereo());
        check (p.checkBusesLayoutSupported (l), "mono in -> stereo out supported");
        l.inputBuses.clear(); l.outputBuses.clear();
        l.inputBuses.add (juce::AudioChannelSet::stereo()); l.outputBuses.add (juce::AudioChannelSet::mono());
        check (! p.checkBusesLayoutSupported (l), "stereo in -> mono out rejected");
    }
}

void learnIntegrationTests()
{
    std::cout << "LEARN in the plugin (library folder -> best IR applied)" << std::endl;
    const auto dir = makeLibrary ("aurora-test-learn", 8);

    AuroraAudioProcessor p;
    p.setPlayConfigDetails (2, 2, fs, 512);
    p.prepareToPlay (fs, 512);
    p.setLibraryFolder (dir);
    check (waitFor ([&] { return ! p.isScanning(); }, 10000) && p.getLibrary()->size() == 8,
           "library loaded in the background: " + p.getLibraryStatus());

    // A pickup that ir5 turns into the target
    const auto lib = p.getLibrary();
    const int truth = lib->indexOf (dir.getChildFile ("sub/ir4.wav")) >= 0 ? lib->indexOf (dir.getChildFile ("sub/ir4.wav")) : 0;
    const auto h = irBands (*lib->get (truth));
    BandArray want {};
    for (int k = 0; k < Bands::count; ++k)
        want[(size_t) k] = Bands::targetDb (Bands::centre (k)) - h[(size_t) k];
    const auto guitar = shapedNoise (want, 20, 5); // ~21.8 s

    setParam (p, "mix", 30.0f);
    p.startLearn (Learn::Mode::pickup);
    check (p.isBusy(), "LEARN listening");
    feed (p, guitar, nullptr, 0, (int) (fs * 16.5));
    check (waitFor ([&] { return ! p.isBusy() && ! p.getSuggestions().empty(); }, 15000), "LEARN finished and compared: " + p.getStatus());
    pump (100);

    const auto list = p.getSuggestions();
    check (list.size() == 3, "3 suggestions");
    if (! list.empty())
    {
        check (list[0].name == "ir4", "best IR = ir4 (got " + list[0].name + ", " + juce::String (list[0].affinity, 1) + "%)");
        check (list[0].affinity > 80.0f, "high affinity: " + juce::String (list[0].affinity, 1) + "%");
        check (p.getCurrentIr() != nullptr && p.getCurrentIr()->name == list[0].name, "best IR loaded automatically");
        check (std::abs (p.params.getRawParameterValue ("mix")->load() - list[0].mix * 100.0f) < 0.6f,
               "MIX set to the suggested blend (" + juce::String (list[0].mix * 100.0f, 0) + "%)");
    }
    check (p.getDryAffinity() < list[0].affinity, "pickup alone scored lower (" + juce::String (p.getDryAffinity(), 1) + "%)");
    check (p.getStatus().contains ("Melhor IR"), "status: " + p.getStatus());

    // Choosing suggestion 2
    p.applySuggestion (1);
    pump (50);
    check (list.size() > 1 && p.getCurrentIr()->name == list[1].name, "clicking suggestion 2 loads it");

    // Session save / restore
    setParam (p, "tone", 2.0f);
    juce::MemoryBlock state;
    p.getStateInformation (state);
    {
        AuroraAudioProcessor q;
        q.setStateInformation (state.getData(), (int) state.getSize());
        waitFor ([&] { return ! q.isScanning(); }, 10000);
        pump (50);
        check (q.getLibraryFolder() == dir, "restores the library folder");
        check (q.getCurrentIr() != nullptr && q.getCurrentIr()->name == list[1].name, "restores the current IR");
        check (q.getSuggestions().size() == 3 && q.getSuggestions()[0].name == list[0].name, "restores the suggestions");
        BandArray s;
        check (q.getLearnedSpectrum (s), "restores the learned spectrum");
        check (std::abs (q.params.getRawParameterValue ("tone")->load() - 2.0f) < 0.01f, "restores the knobs");
        q.applySuggestion (0); // IR loaded again from its file
        check (q.getCurrentIr() != nullptr && q.getCurrentIr()->name == list[0].name, "suggestion works after reload (loaded from file)");
    }

    // Pickup + microphone in a stereo track: creates and saves this guitar's IR
    const auto truthIr = synthIr (6, 1500);
    const auto pickup = whiteNoise ((int) (fs * 22), 21, 0.2f);
    const auto mic = convolveDirect (pickup, truthIr);
    p.startLearn (Learn::Mode::pickupAndMic);
    feed (p, pickup, &mic, 0, (int) pickup.size());
    check (waitFor ([&] { return ! p.isBusy() && p.getStatus().contains ("criado"); }, 15000), "mic LEARN: " + p.getStatus());
    pump (100);
    const auto own = p.getSuggestions();
    check (own.size() == 1 && own[0].file.existsAsFile() && own[0].file.getParentDirectory() == dir.getChildFile ("Meus IRs Aurora"),
           "IR saved in <biblioteca>/Meus IRs Aurora: " + (own.empty() ? juce::String() : own[0].file.getFileName()));
    check (p.getCurrentIr() != nullptr && p.getCurrentIr()->file == (own.empty() ? juce::File() : own[0].file), "own IR loaded");
    check (p.params.getRawParameterValue ("input")->load() > 0.5f, "input switched to channel 1 only (mic not mixed in)");
    check (p.getLibrary()->size() == 9, "own IR added to the library");
    if (! own.empty())
    {
        juce::String err;
        auto saved = IrLibrary::loadFile (own[0].file, err);
        auto ref = IrLibrary::fromSamples (truthIr.data(), (int) truthIr.size(), fs, "ref", {});
        check (saved != nullptr && shapeError (irBands (*saved), irBands (*ref)) < 1.0f,
               "saved IR sounds like the real pickup->mic path (" + juce::String (saved != nullptr ? shapeError (irBands (*saved), irBands (*ref)) : 99.0f, 2) + " dB)");
    }

    // Mic LEARN on a mono track
    {
        AuroraAudioProcessor m;
        m.setPlayConfigDetails (1, 2, fs, 512);
        m.prepareToPlay (fs, 512);
        m.startLearn (Learn::Mode::pickupAndMic);
        check (! m.isBusy() && m.getStatus().contains (pt ("faixa estéreo")), "mono track -> " + m.getStatus());
    }

    // The editor opens and shows everything
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        check (editor != nullptr && editor->getWidth() > 800, "editor opens");
        pump (100);
    }
    dir.deleteRecursively();
}
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode (argc > 1 ? argv[1] : "");

    if (mode == "--snapshot" && argc > 2)
    {
        // Simulated session: library + LEARN, then a picture of the window
        const auto dir = makeLibrary (pt ("Violões acousticir (exemplo)"), 8);
        const char* names[] = { "Dreadnought mogno - KM184", "Grand Auditorium - fita", "Slope shoulder - C414",
                                "Jumbo - par estéreo", "Folk - SM81", "Dreadnought jacarandá - U87", "OM - DPA 4011",
                                "Clássico nylon - KM184" };
        for (int v = 0; v < 8; ++v)
        {
            auto f = dir.getChildFile ("ir" + juce::String (v) + ".wav");
            if (! f.existsAsFile()) f = dir.getChildFile ("sub/ir" + juce::String (v) + ".wav");
            f.moveFileTo (dir.getChildFile (pt (names[v]) + ".wav"));
        }
        AuroraAudioProcessor p;
        p.setPlayConfigDetails (2, 2, fs, 512);
        p.prepareToPlay (fs, 512);
        p.setLibraryFolder (dir);
        waitFor ([&] { return ! p.isScanning(); }, 10000);
        BandArray piezo {};
        for (int k = 0; k < Bands::count; ++k)
        {
            const float f = Bands::centre (k);
            // A typical piezo: thin lows, a 'quack' bump around 1-3 kHz, harsh top
            piezo[(size_t) k] = Bands::targetDb (f) - 8.0f * std::exp (-std::pow (std::log2 (f / 110.0f), 2.0f))
                              + 6.0f * std::exp (-std::pow (std::log2 (f / 1800.0f) / 0.8f, 2.0f)) + 2.0f * std::log2 (f / 1000.0f);
        }
        const auto guitar = shapedNoise (piezo, 20, 9);
        p.startLearn (Learn::Mode::pickup);
        feed (p, guitar, nullptr, 0, (int) (fs * 16.5));
        waitFor ([&] { return ! p.isBusy() && ! p.getSuggestions().empty(); }, 15000);
        pump (100);

        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        pump (200);
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        juce::File out { juce::String (argv[2]) };
        out.deleteFile();
        juce::PNGImageFormat png;
        if (auto stream = out.createOutputStream())
            png.writeImageToStream (image, *stream);
        std::cout << "snapshot: " << out.getFullPathName() << std::endl;
        editor.reset();
        dir.deleteRecursively();
        return 0;
    }

    std::cout << "Aurora IR Match tests" << std::endl;
    bandTests();
    libraryTests();
    matcherTests();
    learnTests();
    processorTests();
    learnIntegrationTests();
    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
