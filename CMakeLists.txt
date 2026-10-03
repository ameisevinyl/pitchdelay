cmake_minimum_required(VERSION 3.22)

# macOS settings must precede project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
option(PITCHDELAY_UNIVERSAL "Build arm64 + x86_64" OFF)
if(PITCHDELAY_UNIVERSAL)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

project(PitchDelay VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd

option(PITCHDELAY_INSTALL_AFTER_BUILD "Copy plugins to ~/Library/Audio/Plug-Ins after build" ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.16.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE Catch2)

# ---- plugin -------------------------------------------------------------
juce_add_plugin(PitchDelay
    COMPANY_NAME "ameisevinyl"
    BUNDLE_ID com.ameisevinyl.pitchdelay
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${PITCHDELAY_INSTALL_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE Amvl
    PLUGIN_CODE Pdly
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "PitchDelay"
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Delay
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

# Sources shared by the plugin and the headless test target.
set(PITCHDELAY_PROCESSOR_SOURCES plugin/PluginProcessor.cpp)

target_sources(PitchDelay PRIVATE
    ${PITCHDELAY_PROCESSOR_SOURCES}
    plugin/PluginEntry.cpp)
target_include_directories(PitchDelay PRIVATE plugin)
target_compile_definitions(PitchDelay PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(PitchDelay
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)

# ---- core (JUCE-free) -----------------------------------------------------
add_library(pitchdelay_core STATIC core/DelayLine.cpp core/DelaySettings.cpp)
target_include_directories(pitchdelay_core PUBLIC core)
target_compile_features(pitchdelay_core PUBLIC cxx_std_17)

enable_testing()
add_executable(pitchdelay_core_tests tests/DelayLineTests.cpp tests/DelaySettingsTests.cpp)
target_link_libraries(pitchdelay_core_tests PRIVATE pitchdelay_core Catch2::Catch2WithMain)
add_test(NAME core COMMAND pitchdelay_core_tests)
