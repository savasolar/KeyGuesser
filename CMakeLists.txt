cmake_minimum_required(VERSION 3.15)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build architectures for macOS")
endif()

project(PPD VERSION 3.0.0)

set(CMAKE_CXX_STANDARD 17)

# JUCE path
if(APPLE)
    set(JUCE_PATH "/Users/admin/Documents/repos/JUCE")
elseif(NOT APPLE)
    set(JUCE_PATH "C:/Program Files/JUCE")
endif()

# Include JUCE
add_subdirectory(${JUCE_PATH} JUCE)

# Conditional formats
if(APPLE)
    set(PLUGIN_FORMATS AU VST3)
elseif(NOT APPLE)
    set(PLUGIN_FORMATS VST3)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
    add_compile_options(
        $<$<CONFIG:Debug>:/MTd>
        $<$<CONFIG:Release>:/MT>
    )
endif()

set(PLUGIN_CODE "KeyG")
set(PLUGIN_PRODUCT_NAME "KeyGuesser")
set(PLUGIN_BUNDLE_ID "com.54v450l4r.KeyGuesser")

# Plugin setup arguments (common)
set(PLUGIN_ARGS
    PLUGIN_MANUFACTURER_CODE "Manu"
    PLUGIN_CODE "${PLUGIN_CODE}"
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "${PLUGIN_PRODUCT_NAME}"
    COMPANY_NAME "54v450l4r"
    BUNDLE_ID "${PLUGIN_BUNDLE_ID}"
    PLUGIN_IS_A_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    IS_MIDI_EFFECT FALSE
    IS_SYNTH FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    MIDI_INPUT_NAME "MIDI Input"
    MIDI_OUTPUT_NAME "MIDI Output"
)

# Plugin setup
juce_add_plugin(PPD ${PLUGIN_ARGS})

target_compile_definitions(PPD
    PRIVATE
        JUCE_DISABLE_VST2_BUILDING=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
)

# Source files
target_sources(PPD PRIVATE
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/core.c
    Source/core.h
    Source/PluginLogger.cpp
    Source/PluginLogger.h
    Source/dr_wav.h
)

set_source_files_properties(Source/core.c PROPERTIES LANGUAGE C)

# Binary data
juce_add_binary_data(BinaryResources SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Resources/resources_clean.json
)

# Additional Libraries

if (APPLE)
    # ONNX
    set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/onnxruntime-osx-x86_64-static_lib-1.25.0")
    target_include_directories(PPD PRIVATE ${ONNXRUNTIME_DIR}/include)
    target_link_libraries(PPD PRIVATE ${ONNXRUNTIME_DIR}/lib/libonnxruntime.a)
    # SRC
    target_include_directories(PPD PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/libsamplerate/include")
    target_link_libraries(PPD PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/libsamplerate/build/src/libsamplerate.a")
elseif(NOT APPLE)
    # ONNX
    set(ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/onnxruntime-win-x64-1.25.0-static-release")
    target_include_directories(PPD PRIVATE ${ONNXRUNTIME_DIR}/include)
    target_link_libraries(PPD PRIVATE ${ONNXRUNTIME_DIR}/lib/onnxruntime.lib)
    # SRC
    target_include_directories(PPD PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/libsamplerate/include")
    target_link_libraries(PPD PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Dependencies/libsamplerate/build/src/Release/samplerate.lib")
endif()

# Link everything
target_link_libraries(PPD PRIVATE
    BinaryResources
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_plugin_client
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
)

juce_generate_juce_header(PPD)