cmake_minimum_required(VERSION 3.21)
project(NeonSynth VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Override with -DJUCE_DIR=... if JUCE is not checked into the project tree
if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/JUCE")
endif()
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

juce_add_plugin(NeonSynth
    VERSION                     1.4.0
    COMPANY_NAME                "AudioDSP"
    COMPANY_WEBSITE             "https://audiodsp.com"
    BUNDLE_ID                   "com.audiodsp.neonsynth"
    PLUGIN_MANUFACTURER_CODE    AuDs
    PLUGIN_CODE                 BaSy
    FORMATS                     Standalone VST3
    PRODUCT_NAME                "NeonSynth"
    ICON_BIG                    "${CMAKE_CURRENT_LIST_DIR}/icon.png"
    ICON_SMALL                  "${CMAKE_CURRENT_LIST_DIR}/icon.png"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

juce_generate_juce_header(NeonSynth)

juce_add_binary_data(NeonSynthResources
    SOURCES
        gui_background.png
)

target_sources(NeonSynth
    PRIVATE
        NeonSynthVoice.h
        NeonSynthEditor.h
        NeonSynthProcessor.h
        NeonSynthProcessor.cpp
        MidiLogger.h
        Visualisers.h
        StepSequencer.h
        SequencerView.h
        HelpOverlay.h
        VoiceEnvelope.h
        Oscillator.h
        SampleData.h
        SampleSlot.h
        SampleLoader.h
        SampleReader.h
        GrainCloud.h
        SampleView.h
)

target_compile_definitions(NeonSynth
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0   # never shipped as VST2; parameters use ParameterID
        JUCE_WEB_BROWSER=0   # plugin has no embedded browser
        JUCE_USE_CURL=0      # plugin has no network I/O
)

target_link_libraries(NeonSynth
    PRIVATE
        NeonSynthResources
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

target_include_directories(NeonSynth
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# ---------------------------------------------------------------------------
# Optional unit tests: the step sequencer engine (StepSequencer.h), the voice
# envelope (VoiceEnvelope.h), the oscillator (Oscillator.h) and the sample
# (SampleData.h, SampleSlot.h, SampleLoader.h). They only need JUCE's audio
# modules, so they build in a few seconds once JUCE is compiled:
#   cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
#   cmake --build build --config Release --target unit_tests
#   ctest --test-dir build -C Release --output-on-failure
# Each executable exits non-zero if any test fails. A new test only needs its
# name added to NEONSYNTH_UNIT_TESTS; CI runs everything ctest knows about.
# ---------------------------------------------------------------------------
option(NEONSYNTH_BUILD_TESTS "Build the unit tests and the render benchmark" OFF)

if(NEONSYNTH_BUILD_TESTS)
    enable_testing()
    set(NEONSYNTH_UNIT_TESTS StepSequencerTests VoiceEnvelopeTests OscillatorTests SampleDataTests SampleReaderTests GranularTests)
    add_custom_target(unit_tests)

    foreach(test ${NEONSYNTH_UNIT_TESTS})
        juce_add_console_app(${test} PRODUCT_NAME "${test}")
        target_sources(${test} PRIVATE tests/${test}.cpp)
        target_compile_definitions(${test} PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
        target_link_libraries(${test} PRIVATE
            juce::juce_audio_basics
            juce::juce_audio_formats
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
        add_dependencies(unit_tests ${test})
        add_test(NAME ${test} COMMAND ${test})
    endforeach()

    # Programs that drive the whole processor directly (no host, no Python):
    #   RenderBenchmark      times processBlock for a few load scenarios
    #   RealtimeSafetyTests  fails if processBlock allocates (also run by ctest)
    foreach(program RenderBenchmark RealtimeSafetyTests)
        juce_add_console_app(${program} PRODUCT_NAME "${program}")
        juce_generate_juce_header(${program})
        target_sources(${program} PRIVATE tests/${program}.cpp NeonSynthProcessor.cpp)
        target_include_directories(${program} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
        target_compile_definitions(${program} PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
        target_link_libraries(${program} PRIVATE
            NeonSynthResources
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
    endforeach()

    add_dependencies(unit_tests RealtimeSafetyTests)
    add_test(NAME RealtimeSafetyTests COMMAND RealtimeSafetyTests)
endif()
