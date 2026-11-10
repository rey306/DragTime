cmake_minimum_required(VERSION 3.22)
project(DragTime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# Installs the plugin into the system plugin folder after each build.
# On Windows that folder needs admin rights; pass -DDRAGTIME_COPY_AFTER_BUILD=OFF
# and copy DragTime.vst3 by hand if the build fails at the copy step.
option(DRAGTIME_COPY_AFTER_BUILD "Copy the plugin to the system plugin folder" ON)

juce_add_plugin(DragTime
    COMPANY_NAME              "MyPlugins"
    PLUGIN_MANUFACTURER_CODE  Mypl
    PLUGIN_CODE               Drgt
    FORMATS                   VST3 AU Standalone
    PRODUCT_NAME              "DragTime"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    COPY_PLUGIN_AFTER_BUILD   ${DRAGTIME_COPY_AFTER_BUILD})

target_sources(DragTime PRIVATE Source/PluginProcessor.cpp)

target_compile_definitions(DragTime PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(DragTime
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
