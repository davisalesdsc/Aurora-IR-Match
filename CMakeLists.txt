cmake_minimum_required(VERSION 3.22)

# macOS settings MUST come before project(), otherwise the plugin requires the build machine's macOS
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary (Apple Silicon + Intel)")

project(AuroraIRMatch VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: downloaded automatically, or a local copy with -DAURORA_JUCE_DIR=/path/to/JUCE
# ---------------------------------------------------------------------------
set(AURORA_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(AURORA_JUCE_DIR)
    add_subdirectory(${AURORA_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(AURORA_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND AURORA_FORMATS AU)
endif()

juce_add_plugin(AuroraIRMatch
    COMPANY_NAME "Aurora"
    BUNDLE_ID "com.aurora.irmatch"
    PLUGIN_MANUFACTURER_CODE Aura
    PLUGIN_CODE Arm1
    PRODUCT_NAME "Aurora IR Match"
    FORMATS ${AURORA_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx EQ
    AU_MAIN_TYPE kAudioUnitType_Effect)

set(AURORA_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ir/IrLibrary.cpp
    Source/ir/Matcher.cpp
    Source/ir/Learn.cpp)

set(AURORA_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_sources(AuroraIRMatch PRIVATE ${AURORA_SOURCES})
target_include_directories(AuroraIRMatch PRIVATE Source)
target_compile_definitions(AuroraIRMatch PUBLIC ${AURORA_DEFINITIONS})
if(MSVC)
    target_compile_options(AuroraIRMatch PRIVATE /utf-8)
endif()

target_link_libraries(AuroraIRMatch
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Tests:  AuroraTests                     -> unit + integration tests
#         AuroraTests --snapshot out.png  -> picture of the plugin window
#         AuroraPluginLoadTest <.vst3|AU>  -> loads the INSTALLED plugin like a DAW
# ---------------------------------------------------------------------------
option(AURORA_BUILD_TESTS "Build the test runners" ON)

if(AURORA_BUILD_TESTS)
    juce_add_console_app(AuroraTests PRODUCT_NAME "AuroraTests")
    target_sources(AuroraTests PRIVATE Tests/AuroraTests.cpp ${AURORA_SOURCES})
    target_include_directories(AuroraTests PRIVATE Source)
    target_compile_definitions(AuroraTests PRIVATE ${AURORA_DEFINITIONS}
        JucePlugin_Name="Aurora IR Match"
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STANDALONE_APPLICATION=1)
    if(MSVC)
        target_compile_options(AuroraTests PRIVATE /utf-8)
    endif()
    target_link_libraries(AuroraTests PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    juce_add_console_app(AuroraPluginLoadTest PRODUCT_NAME "AuroraPluginLoadTest")
    target_sources(AuroraPluginLoadTest PRIVATE Tests/PluginLoadTest.cpp)
    target_compile_definitions(AuroraPluginLoadTest PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    if(MSVC)
        target_compile_options(AuroraPluginLoadTest PRIVATE /utf-8)
    endif()
    target_link_libraries(AuroraPluginLoadTest PRIVATE
        juce::juce_audio_utils
        juce::juce_recommended_config_flags)
endif()
