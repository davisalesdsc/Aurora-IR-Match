#include "PluginEditor.h"

using namespace aurora;

namespace
{
    namespace col
    {
        const juce::Colour bg       { 0xff15110e };
        const juce::Colour panel    { 0xff211b16 };
        const juce::Colour edge     { 0xff3a3029 };
        const juce::Colour text     { 0xfff1e6d8 };
        const juce::Colour dim      { 0xffa6968a };
        const juce::Colour amber    { 0xffeaa64d };
        const juce::Colour pickup   { 0xff7fa8c9 };
        const juce::Colour good     { 0xff8fd18a };
    }

    constexpr int W = 940, H = 640;

    float freqToX (float hz, juce::Rectangle<float> r)
    {
        const float t = std::log (hz / 40.0f) / std::log (20000.0f / 40.0f);
        return r.getX() + t * r.getWidth();
    }

    /** Offset that lines a curve up with the target (weighted mean of the difference). */
    float alignToTarget (const BandArray& b)
    {
        double sw = 0.0, sd = 0.0;
        for (int k = 0; k < Bands::count; ++k)
        {
            sw += Bands::weight (k);
            sd += Bands::weight (k) * (Bands::targetDb (Bands::centre (k)) - b[(size_t) k]);
        }
        return (float) (sd / sw);
    }

    void styleLabel (juce::Label& l, float size, juce::Colour c, juce::Justification j = juce::Justification::centredLeft)
    {
        l.setFont (juce::FontOptions (size));
        l.setColour (juce::Label::textColourId, c);
        l.setJustificationType (j);
        l.setMinimumHorizontalScale (0.7f);
    }
}

//==============================================================================
AuroraLookAndFeel::AuroraLookAndFeel()
{
    setColour (juce::TextButton::buttonColourId, col::edge);
    setColour (juce::TextButton::textColourOffId, col::text);
    setColour (juce::TextButton::textColourOnId, col::bg);
    setColour (juce::ComboBox::backgroundColourId, col::panel);
    setColour (juce::ComboBox::outlineColourId, col::edge);
    setColour (juce::ComboBox::textColourId, col::text);
    setColour (juce::ComboBox::arrowColourId, col::amber);
    setColour (juce::PopupMenu::backgroundColourId, col::panel);
    setColour (juce::PopupMenu::textColourId, col::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, col::amber);
    setColour (juce::PopupMenu::highlightedTextColourId, col::bg);
    setColour (juce::Slider::textBoxTextColourId, col::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId, col::dim);
}

void AuroraLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float start, float end, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (6.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
    const auto c = bounds.getCentre();
    const float angle = start + pos * (end - start);
    const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);

    juce::Path track;
    track.addCentredArc (c.x, c.y, radius - 3.0f, radius - 3.0f, 0.0f, start, end, true);
    g.setColour (col::edge);
    g.strokePath (track, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bipolar knobs (tone) fill from the centre
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 && std::abs (s.getMinimum() + s.getMaximum()) < 1.0e-6;
    const float from = bipolar ? (start + end) * 0.5f : start;
    juce::Path value;
    value.addCentredArc (c.x, c.y, radius - 3.0f, radius - 3.0f, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (accent);
    g.strokePath (value, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float body = radius - 11.0f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3b322b), c.x, c.y - body, juce::Colour (0xff1c1713), c.x, c.y + body, false));
    g.fillEllipse (c.x - body, c.y - body, body * 2.0f, body * 2.0f);
    g.setColour (col::edge.brighter (0.2f));
    g.drawEllipse (c.x - body, c.y - body, body * 2.0f, body * 2.0f, 1.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -body + 4.0f, 3.0f, body * 0.45f, 1.5f);
    g.setColour (col::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

void AuroraLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& base, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? col::amber : base;
    if (! b.isEnabled()) c = c.withMultipliedAlpha (0.4f);
    else if (down) c = c.brighter (0.25f);
    else if (over) c = c.brighter (0.12f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (c.brighter (0.25f).withAlpha (0.6f));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
}

juce::Font AuroraLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (14.0f, buttonHeight * 0.42f), juce::Font::bold);
}

void AuroraLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    g.setColour (on ? juce::Colour (0xffd0583f) : (over ? col::edge.brighter (0.1f) : col::edge));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (on ? juce::Colours::white : col::dim);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (b.getButtonText(), r, juce::Justification::centred);
}

//==============================================================================
void SpectrumView::setCurves (bool hasIn, const BandArray& in, bool hasRes, const BandArray& res)
{
    hasInput = hasIn;
    hasResult = hasRes;
    input = in;
    result = res;
    repaint();
}

void SpectrumView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour (col::panel);
    g.fillRoundedRectangle (area, 8.0f);

    auto plot = area.reduced (14.0f, 12.0f).withTrimmedBottom (14.0f).withTrimmedTop (16.0f);
    const float topDb = 8.0f, bottomDb = -42.0f;
    auto yOf = [&] (float db) { return plot.getY() + (topDb - db) / (topDb - bottomDb) * plot.getHeight(); };

    // Grid
    g.setFont (juce::FontOptions (10.0f));
    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = freqToX (f, plot);
        g.setColour (col::edge.withAlpha (0.7f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (col::dim);
        g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f),
                    juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 13.0f), juce::Justification::centred);
    }
    for (float db = 0.0f; db >= bottomDb; db -= 10.0f)
    {
        g.setColour (col::edge.withAlpha (0.5f));
        g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
    }

    auto curve = [&] (auto dbAt) {
        juce::Path p;
        for (int k = 0; k < Bands::count; ++k)
        {
            const float x = freqToX (Bands::centre (k), plot);
            const float y = juce::jlimit (plot.getY(), plot.getBottom(), yOf (dbAt (k)));
            if (k == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        return p;
    };

    // Target (dashed)
    {
        auto p = curve ([] (int k) { return Bands::targetDb (Bands::centre (k)); });
        juce::Path dashed;
        const float dashes[] = { 5.0f, 4.0f };
        juce::PathStrokeType (1.5f).createDashedStroke (dashed, p, dashes, 2);
        g.setColour (col::text.withAlpha (0.75f));
        g.fillPath (dashed);
    }
    if (hasInput)
    {
        const float off = alignToTarget (input);
        g.setColour (col::pickup);
        g.strokePath (curve ([&] (int k) { return input[(size_t) k] + off; }), juce::PathStrokeType (1.8f));
    }
    if (hasResult)
    {
        const float off = alignToTarget (result);
        auto p = curve ([&] (int k) { return result[(size_t) k] + off; });
        g.setColour (col::amber);
        g.strokePath (p, juce::PathStrokeType (2.6f));
    }

    // Legend
    auto legend = area.reduced (14.0f, 6.0f).removeFromTop (16.0f);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    auto item = [&] (juce::Colour c, const juce::String& t, bool dashedLine) {
        auto r = legend.removeFromLeft (t.length() * 7.0f + 34.0f);
        g.setColour (c);
        if (dashedLine)
            for (int i = 0; i < 3; ++i)
                g.fillRect (r.getX() + i * 7.0f, r.getCentreY() - 1.0f, 4.0f, 2.0f);
        else
            g.fillRect (r.getX(), r.getCentreY() - 1.0f, 18.0f, 2.5f);
        g.setColour (col::dim);
        g.drawText (t, r.withTrimmedLeft (24.0f), juce::Justification::centredLeft);
    };
    item (col::text, pt ("ALVO (violão microfonado)"), true);
    item (col::pickup, "SEU CAPTADOR", false);
    item (col::amber, "RESULTADO", false);

    if (! hasInput)
    {
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (pt ("Aperte LEARN e toque o violão para ver o som do seu captador aqui."), plot.translated (0.0f, 20.0f),
                    juce::Justification::centred);
    }
}

//==============================================================================
void SuggestionCard::set (int r, const juce::String& n, float m, float a, bool sel)
{
    rank = r; name = n; mix = m; affinity = a; selected = sel; empty = false;
    setEnabled (true);
    repaint();
}

void SuggestionCard::clear()
{
    empty = true;
    setEnabled (false);
    repaint();
}

void SuggestionCard::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (selected ? col::amber.withAlpha (0.16f) : (over ? col::edge.withAlpha (0.6f) : col::panel));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (selected ? col::amber : col::edge);
    g.drawRoundedRectangle (r, 8.0f, selected ? 2.0f : 1.0f);

    auto inner = r.reduced (10.0f, 8.0f);
    if (empty)
    {
        g.setColour (col::dim.withAlpha (0.6f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (pt ("Sugestão ") + juce::String (rank > 0 ? rank : 0), inner, juce::Justification::centred);
        return;
    }

    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (selected ? col::amber : col::dim);
    g.drawText (juce::String (rank) + pt ("º") + (selected ? pt ("  ·  EM USO") : ""), inner.removeFromTop (14.0f), juce::Justification::centredLeft);

    g.setColour (col::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawFittedText (name, inner.removeFromTop (34.0f).toNearestInt(), juce::Justification::topLeft, 2, 0.8f);

    auto bottom = inner.removeFromBottom (16.0f);
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (col::dim);
    g.drawText ("IR " + juce::String (juce::roundToInt (mix * 100.0f)) + "%", bottom, juce::Justification::centredRight);

    const auto bar = bottom.removeFromLeft (bottom.getWidth() * 0.62f).withSizeKeepingCentre (bottom.getWidth() * 0.62f, 6.0f);
    g.setColour (col::edge);
    g.fillRoundedRectangle (bar, 3.0f);
    g.setColour (affinity >= 70.0f ? col::good : col::amber);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, affinity / 100.0f)), 3.0f);
    g.setColour (col::text);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (juce::String (juce::roundToInt (affinity)) + "%", inner.removeFromBottom (16.0f), juce::Justification::centredLeft);
}

//==============================================================================
AuroraAudioProcessorEditor::AuroraAudioProcessorEditor (AuroraAudioProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&lnf);

    styleLabel (folderLabel, 12.0f, col::text);
    styleLabel (libraryStatus, 12.0f, col::dim);
    styleLabel (learnHint, 12.0f, col::dim, juce::Justification::topLeft);
    styleLabel (statusLabel, 13.0f, col::text);
    styleLabel (dryLabel, 12.0f, col::dim, juce::Justification::centredRight);
    styleLabel (irLabel, 12.0f, col::text);
    learnHint.setText (pt ("Toque por uns 15 segundos: acordes, dedilhado, notas graves e agudas, "
                       "no volume em que você costuma tocar."), juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*> { &folderLabel, &libraryStatus, &learnHint, &statusLabel, &dryLabel,
                                                            &irLabel, &chooseFolderButton, &downloadButton, &learnPickupButton,
                                                            &learnMicButton, &finishButton, &cancelButton, &spectrum,
                                                            &loadIrButton, &clearIrButton, &inputBox, &bypassButton })
        addAndMakeVisible (*c);

    learnPickupButton.setColour (juce::TextButton::buttonColourId, col::amber.darker (0.35f));
    learnMicButton.setColour (juce::TextButton::buttonColourId, col::edge.brighter (0.1f));
    finishButton.setColour (juce::TextButton::buttonColourId, col::good.darker (0.5f));

    chooseFolderButton.onClick = [this] { chooseFolder(); };
    downloadButton.onClick = [] { juce::URL ("https://www.acousticir.ovh/").launchInDefaultBrowser(); };
    downloadButton.setTooltip (pt ("Abre o site acousticir.ovh: baixe os IRs dos violões que quiser e coloque na pasta da biblioteca."));
    learnPickupButton.onClick = [this] { proc.startLearn (Learn::Mode::pickup); refresh(); };
    learnMicButton.onClick = [this] { proc.startLearn (Learn::Mode::pickupAndMic); refresh(); };
    finishButton.onClick = [this] { proc.finishLearn(); };
    cancelButton.onClick = [this] { proc.cancelLearn(); refresh(); };
    loadIrButton.onClick = [this] { chooseIrFile(); };
    clearIrButton.onClick = [this] { proc.clearIr(); };

    for (int i = 0; i < 3; ++i)
    {
        addAndMakeVisible (cards[(size_t) i]);
        cards[(size_t) i].onClick = [this, i] { proc.applySuggestion (i); };
    }

    auto setupKnob = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, juce::Colour c) {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
        s.setColour (juce::Slider::rotarySliderFillColourId, c);
        addAndMakeVisible (s);
        styleLabel (l, 12.0f, col::dim, juce::Justification::centred);
        l.setText (name, juce::dontSendNotification);
        addAndMakeVisible (l);
    };
    setupKnob (mixKnob, mixLabel, pt ("MIX  (LINHA ↔ IR)"), col::amber);
    setupKnob (toneKnob, toneLabel, pt ("TOM  (ESCURO ↔ BRILHO)"), col::pickup);
    setupKnob (outputKnob, outputLabel, pt ("SAÍDA"), col::text);

    inputBox.addItemList ({ "Entrada: L + R", pt ("Entrada: só canal 1 (L)") }, 1);

    mixAttach = std::make_unique<SliderAttachment> (proc.params, "mix", mixKnob);
    toneAttach = std::make_unique<SliderAttachment> (proc.params, "tone", toneKnob);
    outputAttach = std::make_unique<SliderAttachment> (proc.params, "output", outputKnob);
    inputAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, "input", inputBox);
    bypassAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.params, "bypass", bypassButton);

    // After the attachments (they install the parameter's own text functions)
    mixKnob.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v)) + "% IR"; };
    toneKnob.textFromValueFunction = [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " dB"; };
    outputKnob.textFromValueFunction = [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " dB"; };
    for (auto* k : { &mixKnob, &toneKnob, &outputKnob })
        k->updateText();

    setSize (W, H);
    refresh();
    startTimerHz (20);
}

AuroraAudioProcessorEditor::~AuroraAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void AuroraAudioProcessorEditor::chooseFolder()
{
    auto start = proc.getLibraryFolder();
    if (! start.isDirectory())
        start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    chooser = std::make_unique<juce::FileChooser> (pt ("Pasta com os IRs de violão (.wav / .aiff)"), start);
    juce::Component::SafePointer<AuroraAudioProcessorEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe] (const juce::FileChooser& fc) {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f.isDirectory())
                                  safe->proc.setLibraryFolder (f);
                          });
}

void AuroraAudioProcessorEditor::chooseIrFile()
{
    auto start = proc.getLibraryFolder();
    if (! start.isDirectory())
        start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Escolha um IR", start, "*.wav;*.aif;*.aiff");
    juce::Component::SafePointer<AuroraAudioProcessorEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc) {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (! f.existsAsFile()) return;
                              juce::String error;
                              if (! safe->proc.loadIrFile (f, error))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Aurora IR Match",
                                                                          pt ("Não foi possível abrir ") + f.getFileName() + ": " + error);
                          });
}

void AuroraAudioProcessorEditor::timerCallback()
{
    const float peak = proc.inputPeak.exchange (0.0f);
    meter = juce::jmax (peak, meter * 0.85f);

    const auto& learn = proc.getLearn();
    const bool nowListening = learn.getState() == Learn::State::listening;
    const float progress = learn.getProgress();
    const float mix = proc.params.getRawParameterValue ("mix")->load();
    const float tone = proc.params.getRawParameterValue ("tone")->load();

    if (proc.changeCount.load() != lastChange || nowListening != listening
        || ! juce::approximatelyEqual (progress, learnProgress) || proc.isScanning())
        refresh();
    if (! juce::approximatelyEqual (mix, lastMix) || ! juce::approximatelyEqual (tone, lastTone))
        updateSpectrum();
    repaint (meterArea);
    repaint (progressArea);
}

void AuroraAudioProcessorEditor::refresh()
{
    lastChange = proc.changeCount.load();
    const auto& learn = proc.getLearn();
    listening = learn.getState() == Learn::State::listening;
    learnProgress = learn.getProgress();

    const auto folder = proc.getLibraryFolder();
    folderLabel.setText (folder == juce::File() ? "Pasta: (nenhuma)" : "Pasta: " + folder.getFullPathName(), juce::dontSendNotification);
    folderLabel.setTooltip (folder.getFullPathName());
    libraryStatus.setText (proc.getLibraryStatus(), juce::dontSendNotification);

    const bool busy = proc.isBusy();
    learnPickupButton.setEnabled (! busy);
    learnMicButton.setEnabled (! busy);
    learnPickupButton.setToggleState (listening && learn.getMode() == Learn::Mode::pickup, juce::dontSendNotification);
    learnMicButton.setToggleState (listening && learn.getMode() == Learn::Mode::pickupAndMic, juce::dontSendNotification);
    finishButton.setVisible (listening);
    cancelButton.setVisible (listening);
    statusLabel.setText (proc.getStatus(), juce::dontSendNotification);

    const auto current = proc.getCurrentIr();
    const auto list = proc.getSuggestions();
    for (int i = 0; i < 3; ++i)
    {
        auto& card = cards[(size_t) i];
        if (i < (int) list.size())
        {
            const auto& s = list[(size_t) i];
            const bool selected = current != nullptr && current->file == s.file;
            card.set (i + 1, s.name, s.mix, s.affinity, selected);
        }
        else
        {
            card.set (i + 1, {}, 0.0f, 0.0f, false);
            card.clear();
        }
    }
    BandArray dummy;
    if (proc.getLearnedSpectrum (dummy) && ! list.empty())
        dryLabel.setText (pt ("Só o captador: ") + juce::String (juce::roundToInt (proc.getDryAffinity())) + "%", juce::dontSendNotification);
    else
        dryLabel.setText ({}, juce::dontSendNotification);

    irLabel.setText (current != nullptr ? "IR atual: " + current->name : "IR atual: nenhum (som do captador)", juce::dontSendNotification);
    clearIrButton.setEnabled (current != nullptr);

    updateSpectrum();
    repaint();
}

void AuroraAudioProcessorEditor::updateSpectrum()
{
    lastMix = proc.params.getRawParameterValue ("mix")->load();
    lastTone = proc.params.getRawParameterValue ("tone")->load();

    BandArray input {};
    const bool hasInput = proc.getLearnedSpectrum (input);
    BandArray result = input;
    bool hasResult = false;
    if (hasInput)
    {
        if (auto ir = proc.getCurrentIr())
            result = Matcher::evaluate (input, *ir, lastMix / 100.0f, 0.0f).resultDb;

        if (std::abs (lastTone) >= 0.05f)
        {
            using AC = juce::dsp::IIR::ArrayCoefficients<float>;
            juce::dsp::IIR::Coefficients<float> lo (AC::makeLowShelf (48000.0, 300.0f, 0.7f, juce::Decibels::decibelsToGain (-lastTone * 0.5f)));
            juce::dsp::IIR::Coefficients<float> hi (AC::makeHighShelf (48000.0, 2500.0f, 0.7f, juce::Decibels::decibelsToGain (lastTone * 0.5f)));
            for (int k = 0; k < Bands::count; ++k)
                result[(size_t) k] += juce::Decibels::gainToDecibels ((float) (lo.getMagnitudeForFrequency (Bands::centre (k), 48000.0)
                                                                              * hi.getMagnitudeForFrequency (Bands::centre (k), 48000.0)));
        }
        hasResult = proc.getCurrentIr() != nullptr || std::abs (lastTone) >= 0.05f;
    }
    spectrum.setCurves (hasInput, input, hasResult, result);
}

//==============================================================================
void AuroraAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    // Header
    g.setColour (col::amber);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText ("AURORA", 24, 14, 140, 34, juce::Justification::centredLeft);
    g.setColour (col::text);
    g.setFont (juce::FontOptions (15.0f));
    g.drawText (pt ("IR MATCH  ·  violão"), 150, 18, 200, 28, juce::Justification::centredLeft);
    g.setColour (col::dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("v" JucePlugin_VersionString, getWidth() - 220, 18, 90, 28, juce::Justification::centredRight);

    auto panel = [&] (juce::Rectangle<int> r, const juce::String& title) {
        g.setColour (col::panel);
        g.fillRoundedRectangle (r.toFloat(), 10.0f);
        g.setColour (col::edge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 10.0f, 1.0f);
        g.setColour (col::amber);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (title, r.reduced (14, 10).removeFromTop (16), juce::Justification::centredLeft);
    };
    panel (libraryArea, pt ("1  ·  BIBLIOTECA DE IRs"));
    panel (learnArea, pt ("2  ·  LEARN"));
    panel (controlsArea, "AJUSTE");

    g.setColour (col::amber);
    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    g.drawText (pt ("3  ·  SUGESTÕES"), resultArea.getX() + 2, resultArea.getY(), 200, 16, juce::Justification::centredLeft);

    // LEARN progress
    if (listening || learnProgress > 0.0f)
    {
        const auto r = progressArea.toFloat();
        g.setColour (col::edge);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (listening ? col::amber : col::good);
        g.fillRoundedRectangle (r.withWidth (r.getWidth() * juce::jlimit (0.0f, 1.0f, learnProgress)), 4.0f);
    }

    // Input meter
    {
        const auto r = meterArea.toFloat();
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText ("ENTRADA", r.withWidth (60.0f), juce::Justification::centredLeft);
        auto bar = r.withTrimmedLeft (62.0f).withSizeKeepingCentre (r.getWidth() - 62.0f, 6.0f);
        g.setColour (col::edge);
        g.fillRoundedRectangle (bar, 3.0f);
        const float db = juce::Decibels::gainToDecibels (meter, -60.0f);
        const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour (db > -1.0f ? juce::Colour (0xffd0583f) : (db < -50.0f ? col::dim : col::good));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * t), 3.0f);
    }
}

void AuroraAudioProcessorEditor::resized()
{
    bypassButton.setBounds (getWidth() - 110, 18, 86, 28);

    auto area = getLocalBounds().reduced (20).withTrimmedTop (44);
    auto left = area.removeFromLeft (300);
    area.removeFromLeft (16);

    libraryArea = left.removeFromTop (156);
    left.removeFromTop (12);
    learnArea = left;

    {
        auto r = libraryArea.reduced (14, 10).withTrimmedTop (22);
        folderLabel.setBounds (r.removeFromTop (20));
        libraryStatus.setBounds (r.removeFromTop (20));
        r.removeFromTop (8);
        auto row = r.removeFromTop (34);
        chooseFolderButton.setBounds (row.removeFromLeft (row.getWidth() / 2 - 4));
        row.removeFromLeft (8);
        downloadButton.setBounds (row);
        r.removeFromTop (6);
        loadIrButton.setBounds (r.removeFromTop (28).removeFromLeft (130));
    }
    {
        auto r = learnArea.reduced (14, 10).withTrimmedTop (24);
        learnPickupButton.setBounds (r.removeFromTop (48));
        r.removeFromTop (8);
        learnMicButton.setBounds (r.removeFromTop (36));
        r.removeFromTop (12);
        progressArea = r.removeFromTop (8);
        r.removeFromTop (10);
        auto row = r.removeFromTop (30);
        finishButton.setBounds (row.removeFromLeft (row.getWidth() / 2 - 4));
        row.removeFromLeft (8);
        cancelButton.setBounds (row);
        r.removeFromTop (8);
        meterArea = r.removeFromTop (16);
        r.removeFromTop (8);
        learnHint.setBounds (r.removeFromTop (60));
    }

    // Right side
    auto right = area;
    spectrum.setBounds (right.removeFromTop (240));
    right.removeFromTop (12);
    resultArea = right.removeFromTop (120);
    {
        auto r = resultArea;
        auto head = r.removeFromTop (18);
        dryLabel.setBounds (head.removeFromRight (240));
        r.removeFromTop (4);
        const int cw = (r.getWidth() - 16) / 3;
        for (int i = 0; i < 3; ++i)
        {
            cards[(size_t) i].setBounds (r.removeFromLeft (cw));
            r.removeFromLeft (8);
        }
    }
    right.removeFromTop (8);
    statusLabel.setBounds (right.removeFromTop (22));
    right.removeFromTop (6);
    controlsArea = right;
    {
        auto r = controlsArea.reduced (14, 10).withTrimmedTop (18);
        auto side = r.removeFromRight (190);
        irLabel.setBounds (side.removeFromTop (34));
        side.removeFromTop (4);
        inputBox.setBounds (side.removeFromTop (26));
        side.removeFromTop (8);
        clearIrButton.setBounds (side.removeFromTop (26).removeFromLeft (90));
        irLabel.setJustificationType (juce::Justification::topLeft);

        const int kw = r.getWidth() / 3;
        juce::Slider* knobs[] = { &mixKnob, &toneKnob, &outputKnob };
        juce::Label* labels[] = { &mixLabel, &toneLabel, &outputLabel };
        for (int i = 0; i < 3; ++i)
        {
            auto k = r.removeFromLeft (kw);
            labels[i]->setBounds (k.removeFromBottom (16));
            knobs[i]->setBounds (k.reduced (6, 0));
        }
    }
}
