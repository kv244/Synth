cmake_minimum_required(VERSION 3.21)
project(NeonSynth VERSION 1.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Override with -DJUCE_DIR=... if JUCE is not checked into the project tree
if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/JUCE")
endif()
add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")

# No fused multiply-add contraction. The grain loop (GrainCloud.h) has scalar, SSE2 and
# AVX2 versions that must render bit-identical audio, so a session sounds the same on
# every machine. A compiler allowed to fuse a*b+c into one FMA (one rounding instead of
# two) does it differently in each version: GCC and Clang do so by default whenever the
# target has FMA (GCC 15 on Ubuntu 26.04 does, out of the box). MSVC doesn't contract
# unless asked (/fp:contract), so it needs no flag.
add_compile_options($<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-ffp-contract=off>)

juce_add_plugin(NeonSynth
    VERSION                     1.7.0
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
        Sculpt.h
        SampleData.h
        SampleSlot.h
        SampleLoader.h
        SampleReader.h
        GrainCloud.h
        SampleView.h
        ShapeView.h
)

target_compile_definitions(NeonSynth
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0   # never shipped as VST2; parameters use ParameterID
        JUCE_WEB_BROWSER=0   # plugin has no embedded browser
        JUCE_USE_CURL=0      # plugin has no network I/O
)

target_link_libraries(NeonSynth
    PRIVATE
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
# Optional unit tests, one executable per file in tests/ (listed in
# NEONSYNTH_UNIT_TESTS below). They only need JUCE's audio modules, so they
# build in a few seconds once JUCE is compiled:
#   cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
#   cmake --build build --config Release --target unit_tests
#   ctest --test-dir build -C Release --output-on-failure
# Each executable exits non-zero if any test fails. A new test only needs its
# name added to NEONSYNTH_UNIT_TESTS; CI runs everything ctest knows about.
# ---------------------------------------------------------------------------
option(NEONSYNTH_BUILD_TESTS "Build the unit tests and the render benchmark" OFF)

if(NEONSYNTH_BUILD_TESTS)
    enable_testing()
    set(NEONSYNTH_UNIT_TESTS StepSequencerTests VoiceEnvelopeTests OscillatorTests ChopTests SculptTests SampleDataTests SampleReaderTests GranularTests)
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
    #   RenderBenchmark       times processBlock for a few load scenarios
    #   RealtimeSafetyTests   fails if processBlock allocates (also run by ctest)
    #   ParameterLayoutTests  fails if a parameter changed other than by being appended (ctest)
    #   EditorTests           drives the editor offscreen with mouse events and paints it (ctest;
    #                         on Linux it needs an X display, e.g. xvfb-run)
    foreach(program RenderBenchmark RealtimeSafetyTests ParameterLayoutTests EditorTests)
        juce_add_console_app(${program} PRODUCT_NAME "${program}")
        juce_generate_juce_header(${program})
        target_sources(${program} PRIVATE tests/${program}.cpp NeonSynthProcessor.cpp)
        target_include_directories(${program} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
        target_compile_definitions(${program} PRIVATE
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_MODAL_LOOPS_PERMITTED=1                          # EditorTests runs the message loop
            NEONSYNTH_TESTS_DIR="${CMAKE_CURRENT_LIST_DIR}/tests")   # ParameterLayoutTests' reference
        target_link_libraries(${program} PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
    endforeach()

    foreach(program RealtimeSafetyTests ParameterLayoutTests EditorTests)
        add_dependencies(unit_tests ${program})
        add_test(NAME ${program} COMMAND ${program})
    endforeach()
endif()
