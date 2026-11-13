// Loads an INSTALLED plugin exactly like a DAW does and exercises it.
//
//   AuroraPluginLoadTest <path-to-.vst3>
//   AuroraPluginLoadTest AU            (macOS: the Audio Unit aufx/Arm1/Aura)
//
// Prints "LOAD OK" or "LOAD FAILED: <reason from the OS / format>" and exits 0 / 1.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>

namespace
{
int fail (const juce::String& why)
{
    std::cout << "LOAD FAILED: " << why << std::endl;
    return 1;
}
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    if (argc < 2)
        return fail ("usage: AuroraPluginLoadTest <plugin.vst3 | AU>");

    const juce::String target (argv[1]);
    juce::AudioPluginFormatManager formats;
    juce::String identifier;

    if (target == "AU")
    {
       #if JUCE_PLUGINHOST_AU && JUCE_MAC
        formats.addFormat (std::make_unique<juce::AudioUnitPluginFormat>());
        identifier = "AudioUnit:Effects/aufx,Arm1,Aura";
       #else
        return fail ("AU hosting is only available on macOS");
       #endif
    }
    else
    {
        formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
        identifier = juce::File (target).getFullPathName();
        if (! juce::File (identifier).exists())
            return fail ("not found: " + identifier);
    }

    auto* format = formats.getFormat (0);
    juce::OwnedArray<juce::PluginDescription> found;
    format->findAllTypesForFile (found, identifier);
    if (found.isEmpty())
        return fail ("the host found no plugin in " + identifier + " (the binary did not load or exposes no plugin)");

    const auto& desc = *found[0];
    std::cout << "found: " << desc.name << " by " << desc.manufacturerName << " (" << desc.pluginFormatName
              << ", version " << desc.version << ", " << desc.numInputChannels << " in / "
              << desc.numOutputChannels << " out)" << std::endl;

    juce::String error;
    auto instance = formats.createPluginInstance (desc, 48000.0, 512, error);
    if (instance == nullptr)
        return fail ("could not instantiate: " + error);

    std::cout << "instantiated: " << instance->getName() << ", latency " << instance->getLatencySamples() << std::endl;

    instance->setRateAndBufferSizeDetails (48000.0, 512);
    instance->prepareToPlay (48000.0, 512);

    // 2 seconds of noise. Without an IR the plugin outputs the pickup signal (L+R)/2 on both sides.
    juce::Random rng (1);
    juce::AudioBuffer<float> buffer (juce::jmax (2, instance->getTotalNumOutputChannels()), 512), copy;
    juce::MidiBuffer midi;
    float worst = 0.0f;
    bool finite = true;
    for (int b = 0; b < 188; ++b)
    {
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample (c, i, rng.nextFloat() * 0.5f - 0.25f);
        copy.makeCopyOf (buffer);
        instance->processBlock (buffer, midi);
        if (b < 20)
            continue; // parameter smoothing
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const float expected = 0.5f * (copy.getSample (0, i) + copy.getSample (1, i));
                finite = finite && std::isfinite (buffer.getSample (c, i));
                worst = juce::jmax (worst, std::abs (buffer.getSample (c, i) - expected));
            }
    }
    std::cout << "processed 2 s of audio, max deviation from (L+R)/2: " << worst << std::endl;

    // Open and close the plugin window
    if (instance->hasEditor())
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (instance->createEditorAndMakeActive());
        if (editor == nullptr)
            return fail ("the plugin window could not be created");
        std::cout << "editor: " << editor->getWidth() << " x " << editor->getHeight() << std::endl;
        juce::Thread::sleep (300);
        editor.reset();
    }

    juce::MemoryBlock state;
    instance->getStateInformation (state);
    instance->setStateInformation (state.getData(), (int) state.getSize());
    std::cout << "state: " << state.getSize() << " bytes" << std::endl;

    instance->releaseResources();
    instance.reset();

    if (! finite || worst > 1.0e-5f)
        return fail ("unexpected audio output");

    std::cout << "LOAD OK" << std::endl;
    return 0;
}
