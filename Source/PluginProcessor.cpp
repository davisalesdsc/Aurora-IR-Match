#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace aurora;

namespace
{
    const juce::Identifier stateId ("AURORA");

    juce::String bandsToString (const BandArray& b)
    {
        juce::StringArray s;
        for (float v : b) s.add (juce::String (v, 2));
        return s.joinIntoString (",");
    }

    bool bandsFromString (const juce::String& text, BandArray& out)
    {
        juce::StringArray s;
        s.addTokens (text, ",", {});
        if (s.size() != Bands::count)
            return false;
        for (int i = 0; i < Bands::count; ++i)
            out[(size_t) i] = s[i].getFloatValue();
        return true;
    }
}

AuroraAudioProcessor::AuroraAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Entrada", juce::AudioChannelSet::stereo(), true)
                          .withOutput (pt ("Saída"), juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, stateId, createLayout())
{
    mixParam = params.getRawParameterValue ("mix");
    toneParam = params.getRawParameterValue ("tone");
    outputParam = params.getRawParameterValue ("output");
    inputParam = params.getRawParameterValue ("input");
    bypassParam = params.getRawParameterValue ("bypass");

    status = pt ("Escolha a pasta de IRs e aperte LEARN tocando o violão.");
    learn.onFinished = [this] { pool.addJob ([this] { learnFinished(); }); };
}

AuroraAudioProcessor::~AuroraAudioProcessor()
{
    learn.onFinished = nullptr;
    learn.cancel();
    pool.removeAllJobs (true, 10000);
}

juce::AudioProcessorValueTreeState::ParameterLayout AuroraAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix (linha / IR)",
                                                       NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
                                                       AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "tone", 1 }, "Tom",
                                                       NormalisableRange<float> (-6.0f, 6.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "output", 1 }, pt ("Saída"),
                                                       NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "input", 1 }, "Entrada",
                                                        StringArray { "L + R", pt ("Só canal 1 (L)") }, 0));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

//==============================================================================
bool AuroraAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    return ! (in == juce::AudioChannelSet::stereo() && out == juce::AudioChannelSet::mono());
}

void AuroraAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentRate = sampleRate;
    const auto block = (juce::uint32) juce::jmax (1, samplesPerBlock);
    mono.setSize (1, (int) block);
    wet.setSize (1, (int) block);

    convolution.prepare ({ sampleRate, block, 1 });
    if (auto ir = getCurrentIr())
    {
        juce::AudioBuffer<float> copy (ir->samples);
        convolution.loadImpulseResponse (std::move (copy), IrLibrary::rate, juce::dsp::Convolution::Stereo::no,
                                         juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    }

    juce::dsp::ProcessSpec spec { sampleRate, block, 1 };
    lowShelf.coefficients = new juce::dsp::IIR::Coefficients<float> (1, 0, 0, 1, 0, 0);
    highShelf.coefficients = new juce::dsp::IIR::Coefficients<float> (1, 0, 0, 1, 0, 0);
    lowShelf.prepare (spec);
    highShelf.prepare (spec);
    currentTone = -100.0f;
    updateTone (toneParam->load());

    mixSmooth.reset (sampleRate, 0.05);
    gainSmooth.reset (sampleRate, 0.05);
    mixSmooth.setCurrentAndTargetValue (mixParam->load() / 100.0f);
    gainSmooth.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    learn.prepare (sampleRate);
}

void AuroraAudioProcessor::updateTone (float toneDb)
{
    if (std::abs (toneDb - currentTone) < 0.01f)
        return;
    currentTone = toneDb;
    // Tilt: darker / brighter around ~900 Hz
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;
    *lowShelf.coefficients = AC::makeLowShelf (currentRate, 300.0f, 0.7f, juce::Decibels::decibelsToGain (-toneDb * 0.5f));
    *highShelf.coefficients = AC::makeHighShelf (currentRate, 2500.0f, 0.7f, juce::Decibels::decibelsToGain (toneDb * 0.5f));
}

void AuroraAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int inCh = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    const int outCh = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());
    if (n == 0 || inCh == 0)
        return;
    if (n > mono.getNumSamples())
    {
        mono.setSize (1, n, false, false, true);
        wet.setSize (1, n, false, false, true);
    }

    // The pickup signal
    const bool sumInputs = inCh > 1 && inputParam->load() < 0.5f;
    float* m = mono.getWritePointer (0);
    juce::FloatVectorOperations::copy (m, buffer.getReadPointer (0), n);
    if (sumInputs)
    {
        juce::FloatVectorOperations::add (m, buffer.getReadPointer (1), n);
        juce::FloatVectorOperations::multiply (m, 0.5f, n);
    }
    inputPeak.store (juce::jmax (inputPeak.load (std::memory_order_relaxed), mono.getMagnitude (0, 0, n)), std::memory_order_relaxed);

    if (learn.getState() == Learn::State::listening)
    {
        if (learn.getMode() == Learn::Mode::pickupAndMic)
            learn.push (buffer.getReadPointer (0), inCh > 1 ? buffer.getReadPointer (1) : nullptr, n);
        else
            learn.push (m, nullptr, n);
    }

    if (bypassParam->load() > 0.5f)
    {
        for (int c = inCh; c < outCh; ++c)
            buffer.copyFrom (c, 0, buffer, 0, 0, n);
        return;
    }

    // Convolved signal
    float* w = wet.getWritePointer (0);
    const bool haveIr = irActive.load();
    if (haveIr)
    {
        juce::FloatVectorOperations::copy (w, m, n);
        juce::dsp::AudioBlock<float> block (wet.getArrayOfWritePointers(), 1, (size_t) n);
        convolution.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

    mixSmooth.setTargetValue (haveIr ? mixParam->load() / 100.0f : 0.0f);
    gainSmooth.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
    for (int i = 0; i < n; ++i)
    {
        const float mix = mixSmooth.getNextValue();
        const float g = gainSmooth.getNextValue();
        const float dry = m[i];
        m[i] = g * (haveIr ? (1.0f - mix) * dry + mix * w[i] : dry);
    }

    updateTone (toneParam->load());
    if (std::abs (currentTone) >= 0.05f)
    {
        juce::dsp::AudioBlock<float> block (mono.getArrayOfWritePointers(), 1, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        lowShelf.process (ctx);
        highShelf.process (ctx);
    }

    for (int c = 0; c < outCh; ++c)
        buffer.copyFrom (c, 0, mono, 0, 0, n);
    for (int c = outCh; c < buffer.getNumChannels(); ++c)
        buffer.clear (c, 0, n);
}

//==============================================================================
void AuroraAudioProcessor::setStatus (const juce::String& s)
{
    {
        std::lock_guard<std::mutex> l (lock);
        status = s;
    }
    ++changeCount;
}

juce::String AuroraAudioProcessor::getStatus() const
{
    if (learn.getState() == Learn::State::listening)
        return learn.getMessage();
    std::lock_guard<std::mutex> l (lock);
    return status;
}

juce::File AuroraAudioProcessor::getLibraryFolder() const
{
    std::lock_guard<std::mutex> l (lock);
    return libraryFolder;
}

std::shared_ptr<const IrLibrary> AuroraAudioProcessor::getLibrary() const
{
    std::lock_guard<std::mutex> l (lock);
    return library;
}

juce::String AuroraAudioProcessor::getLibraryStatus() const
{
    if (scanning.load())
        return pt ("Lendo IRs… ") + juce::String (juce::roundToInt (scanProgress.load() * 100.0f)) + "%";
    const auto lib = getLibrary();
    const auto folder = getLibraryFolder();
    if (folder == juce::File())
        return "Nenhuma pasta escolhida";
    if (! folder.isDirectory())
        return pt ("Pasta não encontrada");
    juce::String s = juce::String (lib->size()) + (lib->size() == 1 ? " IR pronto" : " IRs prontos");
    if (lib->getErrors().size() > 0)
        s << " (" << lib->getErrors().size() << (lib->getErrors().size() == 1 ? " arquivo ignorado)" : " arquivos ignorados)");
    return s;
}

void AuroraAudioProcessor::setLibraryFolder (const juce::File& folder)
{
    int generation;
    {
        std::lock_guard<std::mutex> l (lock);
        libraryFolder = folder;
        generation = ++scanGeneration;
    }
    scanning = true;
    scanProgress = 0.0f;
    ++changeCount;

    pool.addJob ([this, folder, generation]
    {
        auto lib = std::make_shared<IrLibrary>();
        auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
        lib->scan (folder, [this, job] (int done, int total)
        {
            scanProgress = total > 0 ? (float) done / (float) total : 1.0f;
            ++changeCount;
            return job == nullptr || ! job->shouldExit();
        });

        bool rematch = false;
        {
            std::lock_guard<std::mutex> l (lock);
            if (generation != scanGeneration)
                return; // another folder was chosen meanwhile
            library = lib;
            rematch = hasSpectrum && suggestions.empty() && lib->size() > 0;
        }
        scanning = false;
        scanProgress = 1.0f;
        ++changeCount;
        if (rematch) // LEARN was done before the library was ready: compare now
            runMatch (true);
    });
}

juce::File AuroraAudioProcessor::getOwnIrFolder() const
{
    const auto lib = getLibraryFolder();
    if (lib.isDirectory())
        return lib.getChildFile ("Meus IRs Aurora");
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Aurora IR Match");
}

//==============================================================================
void AuroraAudioProcessor::startLearn (Learn::Mode mode)
{
    if (matching.load())
        return;
    if (mode == Learn::Mode::pickupAndMic && getTotalNumInputChannels() < 2)
    {
        setStatus (pt ("Use uma faixa estéreo: canal 1 = captador, canal 2 = microfone."));
        return;
    }
    learn.start (mode, mode == Learn::Mode::pickup ? pickupLearnSeconds : micLearnSeconds);
    ++changeCount;
}

void AuroraAudioProcessor::cancelLearn()
{
    learn.cancel();
    setStatus ("LEARN cancelado.");
}

void AuroraAudioProcessor::learnFinished()
{
    if (learn.getState() != Learn::State::finished)
    {
        setStatus (learn.getMessage());
        return;
    }

    const auto spectrum = learn.getPickupSpectrum();
    {
        std::lock_guard<std::mutex> l (lock);
        learnedSpectrum = spectrum;
        hasSpectrum = true;
        dryAffinity = Matcher::scoreDry (spectrum).affinity;
    }

    if (learn.getMode() == Learn::Mode::pickup)
    {
        if (getLibrary()->size() == 0)
        {
            {
                std::lock_guard<std::mutex> l (lock);
                suggestions.clear();
            }
            setStatus (scanning.load() ? pt ("Som aprendido. Comparando assim que a biblioteca terminar de carregar…")
                                       : pt ("Som aprendido, mas a biblioteca está vazia: escolha a pasta com os IRs."));
            return;
        }
        runMatch (true);
        return;
    }

    // Pickup + microphone: this guitar's own IR
    const auto samples = learn.getLearnedIr();
    const auto folder = getOwnIrFolder();
    folder.createDirectory();
    const auto name = pt ("Meu violão ") + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    const auto file = folder.getChildFile (juce::File::createLegalFileName (name) + ".wav");
    auto ir = IrLibrary::fromSamples (samples.data(), (int) samples.size(), learn.getSampleRate(), name, file);
    if (ir == nullptr)
    {
        setStatus (pt ("Não foi possível criar o IR. Confira o captador no canal 1 e o microfone no canal 2."));
        return;
    }

    // Save it (24-bit WAV, 48 kHz) so it can be reused in other sessions
    bool saved = false;
    {
        juce::WavAudioFormat wav;
        file.deleteFile();
        if (auto stream = std::unique_ptr<juce::OutputStream> (file.createOutputStream()))
        {
            const auto options = juce::AudioFormatWriterOptions().withSampleRate (IrLibrary::rate)
                                                                 .withNumChannels (1)
                                                                 .withBitsPerSample (24);
            if (auto writer = wav.createWriterFor (stream, options))
                saved = writer->writeFromAudioSampleBuffer (ir->samples, 0, ir->samples.getNumSamples());
        }
    }

    {
        std::lock_guard<std::mutex> l (lock);
        auto lib = std::make_shared<IrLibrary> (*library);
        lib->add (ir);
        library = lib;
        Suggestion s;
        s.name = name;
        s.file = file;
        s.mix = 1.0f;
        s.affinity = Matcher::evaluate (spectrum, *ir, 1.0f, 0.0f).affinity;
        s.ir = ir;
        suggestions = { s };
    }
    setIr (ir);
    setMixParam (1.0f);
    // From now on only the pickup (channel 1) goes through the IR; the mic is no longer mixed in
    juce::MessageManager::callAsync ([weak = juce::WeakReference<AuroraAudioProcessor> (this)]
    {
        if (auto* p = weak.get())
            if (auto* param = p->params.getParameter ("input"))
                param->setValueNotifyingHost (1.0f);
    });

    setStatus (saved ? pt ("IR do seu violão criado! Salvo em: ") + file.getFullPathName()
                     : pt ("IR do seu violão criado (não foi possível salvar o arquivo em ") + folder.getFullPathName() + ").");
}

void AuroraAudioProcessor::runMatch (bool applyBest)
{
    matching = true;
    BandArray spectrum;
    {
        std::lock_guard<std::mutex> l (lock);
        spectrum = learnedSpectrum;
    }
    const auto lib = getLibrary();
    setStatus ("Comparando com " + juce::String (lib->size()) + pt (" IRs…"));

    const auto results = Matcher::match (spectrum, *lib, 0.0f, 3);
    std::vector<Suggestion> list;
    for (const auto& r : results)
    {
        Suggestion s;
        s.ir = lib->get (r.index);
        if (s.ir == nullptr)
            continue;
        s.name = s.ir->name;
        s.file = s.ir->file;
        s.mix = r.mix;
        s.affinity = r.affinity;
        list.push_back (s);
    }

    float dry;
    {
        std::lock_guard<std::mutex> l (lock);
        suggestions = list;
        dry = dryAffinity;
    }
    matching = false;

    if (list.empty())
    {
        setStatus (pt ("Nenhum IR válido na biblioteca."));
        return;
    }
    if (applyBest)
    {
        setIr (list.front().ir);
        setMixParam (list.front().mix);
    }
    setStatus ("Melhor IR: " + list.front().name + pt (" — ") + juce::String (juce::roundToInt (list.front().affinity))
               + pt ("% de equilíbrio (só o captador: ") + juce::String (juce::roundToInt (dry)) + "%)");
}

std::vector<AuroraAudioProcessor::Suggestion> AuroraAudioProcessor::getSuggestions() const
{
    std::lock_guard<std::mutex> l (lock);
    return suggestions;
}

float AuroraAudioProcessor::getDryAffinity() const
{
    std::lock_guard<std::mutex> l (lock);
    return dryAffinity;
}

bool AuroraAudioProcessor::getLearnedSpectrum (BandArray& out) const
{
    std::lock_guard<std::mutex> l (lock);
    out = learnedSpectrum;
    return hasSpectrum;
}

void AuroraAudioProcessor::applySuggestion (int index)
{
    Suggestion s;
    {
        std::lock_guard<std::mutex> l (lock);
        if (! juce::isPositiveAndBelow (index, (int) suggestions.size()))
            return;
        s = suggestions[(size_t) index];
    }
    if (s.ir == nullptr)
    {
        juce::String error;
        s.ir = IrLibrary::loadFile (s.file, error);
        if (s.ir == nullptr)
        {
            setStatus (pt ("Não foi possível abrir ") + s.file.getFileName() + ": " + error);
            return;
        }
        std::lock_guard<std::mutex> l (lock);
        if (index < (int) suggestions.size())
            suggestions[(size_t) index].ir = s.ir;
    }
    setIr (s.ir);
    setMixParam (s.mix);
    setStatus ("IR: " + s.name + pt (" — ") + juce::String (juce::roundToInt (s.affinity)) + pt ("% de equilíbrio"));
}

void AuroraAudioProcessor::setMixParam (float mix01)
{
    auto apply = [weak = juce::WeakReference<AuroraAudioProcessor> (this), mix01]
    {
        if (auto* p = weak.get())
            if (auto* param = p->params.getParameter ("mix"))
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (param->convertTo0to1 (mix01 * 100.0f));
                param->endChangeGesture();
            }
    };
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && juce::MessageManager::getInstance()->isThisTheMessageThread())
        apply();
    else
        juce::MessageManager::callAsync (apply);
}

//==============================================================================
bool AuroraAudioProcessor::loadIrFile (const juce::File& file, juce::String& error)
{
    auto ir = IrLibrary::loadFile (file, error);
    if (ir == nullptr)
        return false;
    setIr (ir);
    setStatus ("IR carregado: " + ir->name);
    return true;
}

void AuroraAudioProcessor::setIr (std::shared_ptr<IrInfo> ir)
{
    if (ir == nullptr)
    {
        clearIr();
        return;
    }
    juce::AudioBuffer<float> copy (ir->samples);
    convolution.loadImpulseResponse (std::move (copy), IrLibrary::rate, juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
    {
        std::lock_guard<std::mutex> l (lock);
        currentIr = ir;
    }
    irActive = true;
    ++changeCount;
}

void AuroraAudioProcessor::clearIr()
{
    irActive = false;
    {
        std::lock_guard<std::mutex> l (lock);
        currentIr = nullptr;
    }
    ++changeCount;
}

std::shared_ptr<IrInfo> AuroraAudioProcessor::getCurrentIr() const
{
    std::lock_guard<std::mutex> l (lock);
    return currentIr;
}

//==============================================================================
void AuroraAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = params.copyState();
    std::lock_guard<std::mutex> l (lock);
    state.setProperty ("libraryFolder", libraryFolder.getFullPathName(), nullptr);
    state.setProperty ("irFile", currentIr != nullptr ? currentIr->file.getFullPathName() : juce::String(), nullptr);
    state.setProperty ("spectrum", hasSpectrum ? bandsToString (learnedSpectrum) : juce::String(), nullptr);
    state.setProperty ("dryAffinity", dryAffinity, nullptr);

    state.removeChild (state.getChildWithName ("Suggestions"), nullptr);
    juce::ValueTree list ("Suggestions");
    for (const auto& s : suggestions)
    {
        juce::ValueTree t ("Suggestion");
        t.setProperty ("name", s.name, nullptr);
        t.setProperty ("file", s.file.getFullPathName(), nullptr);
        t.setProperty ("mix", s.mix, nullptr);
        t.setProperty ("affinity", s.affinity, nullptr);
        list.appendChild (t, nullptr);
    }
    state.appendChild (list, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void AuroraAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (stateId))
        return;
    const auto state = juce::ValueTree::fromXml (*xml);
    params.replaceState (state);

    std::vector<Suggestion> list;
    for (const auto& t : state.getChildWithName ("Suggestions"))
    {
        Suggestion s;
        s.name = t["name"].toString();
        s.file = juce::File (t["file"].toString());
        s.mix = (float) t["mix"];
        s.affinity = (float) t["affinity"];
        list.push_back (s);
    }
    BandArray spectrum {};
    const bool gotSpectrum = bandsFromString (state["spectrum"].toString(), spectrum);
    {
        std::lock_guard<std::mutex> l (lock);
        suggestions = list;
        hasSpectrum = gotSpectrum;
        learnedSpectrum = spectrum;
        dryAffinity = (float) state["dryAffinity"];
    }

    const juce::String folder = state["libraryFolder"].toString();
    if (folder.isNotEmpty() && juce::File::isAbsolutePath (folder))
        setLibraryFolder (juce::File (folder));

    const juce::String irPath = state["irFile"].toString();
    if (irPath.isNotEmpty() && juce::File::isAbsolutePath (irPath))
    {
        juce::String error;
        if (! loadIrFile (juce::File (irPath), error))
            setStatus (pt ("O IR desta sessão não foi encontrado: ") + irPath);
    }
    else
    {
        clearIr();
    }
    ++changeCount;
}

juce::AudioProcessorEditor* AuroraAudioProcessor::createEditor()
{
    return new AuroraAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AuroraAudioProcessor();
}
