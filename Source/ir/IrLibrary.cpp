#include "IrLibrary.h"

namespace aurora
{
namespace
{
    void computeResponse (IrInfo& ir)
    {
        juce::dsp::FFT fft (IrLibrary::fftOrder);
        std::vector<std::complex<float>> in ((size_t) IrLibrary::fftSize), out ((size_t) IrLibrary::fftSize);
        const int n = juce::jmin (ir.samples.getNumSamples(), IrLibrary::fftSize);
        for (int i = 0; i < n; ++i)
            in[(size_t) i] = ir.samples.getSample (0, i);
        fft.perform (in.data(), out.data(), false);
        ir.response.assign (out.begin(), out.begin() + IrLibrary::fftSize / 2 + 1);
    }

    /** Mean power gain between 200 Hz and 5 kHz. */
    double midBandPower (const IrInfo& ir)
    {
        const double binHz = IrLibrary::rate / IrLibrary::fftSize;
        const int b0 = (int) (200.0 / binHz), b1 = (int) (5000.0 / binHz);
        double sum = 0.0;
        for (int b = b0; b <= b1; ++b)
            sum += std::norm (ir.response[(size_t) b]);
        return sum / (b1 - b0 + 1);
    }
}

std::shared_ptr<IrInfo> IrLibrary::fromSamples (const float* data, int numSamples, double sampleRate,
                                                const juce::String& name, const juce::File& file)
{
    if (numSamples < 8 || sampleRate <= 0.0)
        return nullptr;

    // Resample to the library rate
    std::vector<float> resampled;
    if (std::abs (sampleRate - rate) < 0.5)
    {
        resampled.assign (data, data + numSamples);
    }
    else
    {
        const double ratio = sampleRate / rate;
        const int outLen = (int) std::ceil (numSamples / ratio);
        std::vector<float> padded (data, data + numSamples);
        padded.resize ((size_t) numSamples + 64, 0.0f); // the interpolator reads a little ahead
        resampled.resize ((size_t) outLen);
        juce::WindowedSincInterpolator interp;
        interp.process (ratio, padded.data(), resampled.data(), outLen);
    }

    // Trim leading silence (anything 60 dB below the peak), keeping a few samples before the onset
    float peak = 0.0f;
    for (float v : resampled) peak = juce::jmax (peak, std::abs (v));
    if (peak <= 1.0e-9f)
        return nullptr;
    int start = 0;
    while (start < (int) resampled.size() && std::abs (resampled[(size_t) start]) < peak * 0.001f)
        ++start;
    start = juce::jmax (0, start - 8);
    const int len = juce::jmin ((int) resampled.size() - start, maxLengthSamples);

    auto ir = std::make_shared<IrInfo>();
    ir->file = file;
    ir->name = name;
    ir->samples.setSize (1, len);
    ir->samples.copyFrom (0, 0, resampled.data() + start, len);

    // Gentle fade-out over the last 5 % so a cut tail never clicks
    const int fade = juce::jmax (1, len / 20);
    for (int i = 0; i < fade; ++i)
        ir->samples.setSample (0, len - fade + i, ir->samples.getSample (0, len - fade + i) * (1.0f - (float) i / fade));

    computeResponse (*ir);
    const double p = midBandPower (*ir);
    if (p <= 1.0e-20)
        return nullptr;
    const float g = (float) (1.0 / std::sqrt (p));
    ir->samples.applyGain (g);
    for (auto& c : ir->response)
        c *= g;
    return ir;
}

std::shared_ptr<IrInfo> IrLibrary::loadFile (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
    {
        error = pt ("formato não reconhecido");
        return nullptr;
    }
    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) reader->sampleRate * 10)
    {
        error = pt ("duração inválida para um IR");
        return nullptr;
    }

    const int n = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buf ((int) reader->numChannels, n);
    reader->read (&buf, 0, n, 0, true, true);

    // Stereo IRs: use the average of the channels
    std::vector<float> mono ((size_t) n, 0.0f);
    for (int c = 0; c < buf.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += buf.getSample (c, i) / (float) buf.getNumChannels();

    auto ir = fromSamples (mono.data(), n, reader->sampleRate, file.getFileNameWithoutExtension(), file);
    if (ir == nullptr)
        error = "arquivo vazio ou silencioso";
    return ir;
}

void IrLibrary::scan (const juce::File& folder, const std::function<bool (int, int)>& progress)
{
    irs.clear();
    errors.clear();
    if (! folder.isDirectory())
        return;

    auto files = folder.findChildFiles (juce::File::findFiles, true, "*.wav;*.WAV;*.aif;*.aiff;*.AIF;*.AIFF");
    files.sort();
    if (files.size() > maxFiles)
        files.removeRange (maxFiles, files.size() - maxFiles);

    for (int i = 0; i < files.size(); ++i)
    {
        if (progress && ! progress (i, files.size()))
            return;
        juce::String error;
        if (auto ir = loadFile (files[i], error))
            irs.push_back (std::move (ir));
        else
            errors.add (files[i].getFileName() + ": " + error);
    }
    if (progress)
        progress (files.size(), files.size());
}

int IrLibrary::indexOf (const juce::File& f) const
{
    for (int i = 0; i < size(); ++i)
        if (irs[(size_t) i]->file == f)
            return i;
    return -1;
}
}
