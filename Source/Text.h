#pragma once

#include <juce_core/juce_core.h>

namespace aurora
{
/** Portuguese text: source literals are UTF-8 (juce::String (const char*) would read them as Latin-1). */
inline juce::String pt (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}
