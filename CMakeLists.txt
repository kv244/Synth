cmake_minimum_required(VERSION 3.21)
project(BasicSynth VERSION 1.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Override with -DJUCE_DIR=... if JUCE is not checked into the project tree
if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/JUCE")
endif()
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

juce_add_plugin(BasicSynth
    VERSION                     1.3.0
    COMPANY_NAME                "AudioDSP"
    COMPANY_WEBSITE             "https://audiodsp.com"
    BUNDLE_ID                   "com.audiodsp.basicsynth"
    PLUGIN_MANUFACTURER_CODE    AuDs
    PLUGIN_CODE                 BaSy
    FORMATS                     Standalone VST3
    PRODUCT_NAME                "BasicSynth"
    ICON_BIG                    "${CMAKE_CURRENT_LIST_DIR}/icon.png"
    ICON_SMALL                  "${CMAKE_CURRENT_LIST_DIR}/icon.png"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

juce_generate_juce_header(BasicSynth)

juce_add_binary_data(BasicSynthResources
    SOURCES
        gui_background.png
)

target_sources(BasicSynth
    PRIVATE
        BasicSynth.h
        BasicSynthEditor.h
        BasicSynthProcessor.h
        BasicSynthProcessor.cpp
        MidiLogger.h
        Visualisers.h
        StepSequencer.h
        SequencerView.h
        HelpOverlay.h
)

target_compile_definitions(BasicSynth
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0   # never shipped as VST2; parameters use ParameterID
        JUCE_WEB_BROWSER=0   # plugin has no embedded browser
        JUCE_USE_CURL=0      # plugin has no network I/O
)

target_link_libraries(BasicSynth
    PRIVATE
        BasicSynthResources
        juce::juce_core
        juce::juce_gui_basics
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_plugin_client
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

target_include_directories(BasicSynth
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# ---------------------------------------------------------------------------
# Optional unit tests for the step sequencer engine (StepSequencer.h).
# It only needs juce_audio_basics, so it builds in a few seconds once JUCE is
# compiled:
#   cmake -B build -DJUCE_DIR=... -DBASICSYNTH_BUILD_TESTS=ON
#   cmake --build build --config Release --target StepSequencerTests
# The executable exits non-zero if any test fails.
# ---------------------------------------------------------------------------
option(BASICSYNTH_BUILD_TESTS "Build the StepSequencer unit tests" OFF)

if(BASICSYNTH_BUILD_TESTS)
    juce_add_console_app(StepSequencerTests PRODUCT_NAME "StepSequencerTests")
    target_sources(StepSequencerTests PRIVATE tests/StepSequencerTests.cpp)
    target_compile_definitions(StepSequencerTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(StepSequencerTests PRIVATE
        juce::juce_audio_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()
