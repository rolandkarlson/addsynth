cmake_minimum_required(VERSION 3.24)
project(AddSynth VERSION 0.1.0)

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
    PLUGIN_CODE Adsy
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "AddSynth")

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
