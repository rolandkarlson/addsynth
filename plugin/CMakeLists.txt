cmake_minimum_required(VERSION 3.24)
project(AddSynth VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

option(ADDSYNTH_COPY_PLUGIN "Copy plugins to the system folders after build" ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(AddSynth
    COMPANY_NAME "Algorytmid"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${ADDSYNTH_COPY_PLUGIN}
    PLUGIN_MANUFACTURER_CODE Sidz
    PLUGIN_CODE Adsl
    BUNDLE_ID "com.algorytmid.addsynth-sine-lab"
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "AddSynth Sine Lab")

target_sources(AddSynth PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)

target_compile_definitions(AddSynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AddSynth
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# A separate host identity keeps the original instrument available in sessions.
juce_add_plugin(AddSynthV2
    VERSION 0.3.1
    COMPANY_NAME "Algorytmid"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${ADDSYNTH_COPY_PLUGIN}
    PLUGIN_MANUFACTURER_CODE Sidz
    PLUGIN_CODE Asl2
    BUNDLE_ID "com.algorytmid.addsynth-sine-lab-2"
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "AddSynth Sine Lab 2")
target_sources(AddSynthV2 PRIVATE src/PluginProcessor.cpp src/PluginEditor.cpp src/PluginEditorV2.cpp)
target_compile_definitions(AddSynthV2 PUBLIC ADDSYNTH_V2=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(AddSynthV2 PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)

# headless smoke test: loads an .addm and renders a note through AddVoice
juce_add_console_app(addm_test PRODUCT_NAME "addm_test")
target_sources(addm_test PRIVATE test/main.cpp)
target_compile_definitions(addm_test PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(addm_test PRIVATE
    juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)

# offscreen editor snapshot for UI review
juce_add_console_app(ui_snapshot PRODUCT_NAME "ui_snapshot")
target_sources(ui_snapshot PRIVATE
    test/snapshot.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)
target_compile_definitions(ui_snapshot PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ui_snapshot PRIVATE
    juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)

# Partial editor integration: drawing, host gestures, state recall and audio.
juce_add_console_app(partials_test PRODUCT_NAME "partials_test")
target_sources(partials_test PRIVATE
    test/partials.cpp
    test/lab.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)
target_compile_definitions(partials_test PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(partials_test PRIVATE
    juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)
enable_testing()
add_test(NAME partials COMMAND partials_test)

juce_add_console_app(ui_snapshot_v2 PRODUCT_NAME "ui_snapshot_v2")
target_sources(ui_snapshot_v2 PRIVATE test/snapshot.cpp src/PluginProcessor.cpp
    src/PluginEditor.cpp src/PluginEditorV2.cpp)
target_compile_definitions(ui_snapshot_v2 PRIVATE ADDSYNTH_V2=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ui_snapshot_v2 PRIVATE juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)

juce_add_console_app(model_controls_test PRODUCT_NAME "model_controls_test")
target_sources(model_controls_test PRIVATE test/model_controls.cpp src/PluginProcessor.cpp
    src/PluginEditor.cpp src/PluginEditorV2.cpp)
target_compile_definitions(model_controls_test PRIVATE ADDSYNTH_V2=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(model_controls_test PRIVATE juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)
add_test(NAME model_controls COMMAND model_controls_test)

foreach(target AddSynth AddSynthV2 ui_snapshot ui_snapshot_v2 partials_test model_controls_test)
    target_sources(${target} PRIVATE src/AudioModelImporter.cpp)
endforeach()

juce_add_console_app(audio_import_test PRODUCT_NAME "audio_import_test")
target_sources(audio_import_test PRIVATE test/audio_import.cpp test/audio_import_analysis.cpp
    test/model_serialization.cpp src/AudioModelImporter.cpp src/PluginProcessor.cpp
    src/PluginEditor.cpp src/PluginEditorV2.cpp)
target_compile_definitions(audio_import_test PRIVATE ADDSYNTH_V2=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(audio_import_test PRIVATE juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags)
add_test(NAME audio_import COMMAND audio_import_test)
