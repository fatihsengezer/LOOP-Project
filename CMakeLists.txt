################################################################################
# CMakeLists.txt — LOOP Audio Engine
# Visual Studio 2022 / CMake 3.24+
#
# AGENT / DEVELOPER SETUP NOTES
# ─────────────────────────────
# 1. Set JUCE_DIR to your JUCE installation path (or use FetchContent below).
# 2. Configure:  cmake -B build -G "Visual Studio 17 2022" -A x64
# 3. Build:      cmake --build build --config Release
#
# Alternatively: open build/LOOP.sln in Visual Studio 2022.
#
# TARGETS PRODUCED
#   LOOPPlayer_VST3          — VST3 plugin (.vst3)
#   LOOPPlayer_Standalone    — Standalone application (.exe)
#   LOOPTests_Roundtrip      — I/O layer unit tests
#   LOOPTests_SequencerTiming— Sequencer + timing unit tests
#   LOOPTests_ZeroCrossing   — Zero-crossing trim unit tests
#
################################################################################

cmake_minimum_required(VERSION 3.24)

project(LOOP VERSION 1.0.0 LANGUAGES CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# ── JUCE ──────────────────────────────────────────────────────────────────────
#
# Option A: FetchContent (downloads JUCE automatically — requires internet)
#
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        7.0.9        # Pin to a known-good version
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

#
# Option B: Local JUCE installation (comment out FetchContent above, uncomment below)
#
# add_subdirectory("C:/JUCE" JUCE)    # Windows
# add_subdirectory("~/JUCE"  JUCE)    # macOS/Linux


# ── Plugin target ─────────────────────────────────────────────────────────────
juce_add_plugin(LOOPPlayer
    VERSION                     "1.0.0"
    PLUGIN_MANUFACTURER_CODE    Loop
    PLUGIN_CODE                 LpPl
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "LOOP Player"
    COMPANY_NAME                "LOOPEngine"
    COMPANY_WEBSITE             ""
    COMPANY_EMAIL               ""
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    VST3_CATEGORIES             "Instrument"
)

target_sources(LOOPPlayer
    PRIVATE
        src/LoopFileWriter.cpp
        src/LoopFileReader.cpp
        src/LoopSequencer.cpp
        src/LoopPlayerProcessor.cpp
        src/LoopPlayerEditor.cpp
        src/LoopCreatorView.cpp
)

target_include_directories(LOOPPlayer PRIVATE include)

target_compile_definitions(LOOPPlayer
    PUBLIC
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        LOOP_AUDIO_BLOCK_SIZE=512
        $<$<CONFIG:Debug>:JUCE_ENABLE_REPAINT_DEBUGGING=0>
        $<$<CONFIG:Debug>:JUCE_CHECK_MEMORY_LEAKS=1>
)

if(MSVC)
    target_compile_options(LOOPPlayer
        PRIVATE
            /W4 /WX /arch:AVX2 /fp:fast /MP /Zc:__cplusplus
            $<$<CONFIG:Release>:/O2 /GL /Zi>
            $<$<CONFIG:RelWithDebInfo>:/O2 /Zi>
            $<$<CONFIG:Debug>:/Od /fsanitize=address>
    )
    target_link_options(LOOPPlayer PRIVATE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:RelWithDebInfo>:/DEBUG /OPT:REF>
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(LOOPPlayer
        PRIVATE
            -Wall -Wextra -Wpedantic -Werror -mavx2 -ffast-math
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Debug>:-O0 -g -fsanitize=address>
    )
    target_link_options(LOOPPlayer PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)
endif()

target_link_libraries(LOOPPlayer
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)


# ─────────────────────────────────────────────────────────────────────────────
# Helper macro — shared settings for all test executables
# ─────────────────────────────────────────────────────────────────────────────
macro(loop_test_target TARGET_NAME)
    target_include_directories(${TARGET_NAME} PRIVATE include)

    target_compile_definitions(${TARGET_NAME}
        PRIVATE
            JUCE_STANDALONE_APPLICATION=1
            JUCE_STRICT_REFCOUNTEDPOINTER=1
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
    )

    if(MSVC)
        target_compile_options(${TARGET_NAME}
            PRIVATE
                /W4 /Zc:__cplusplus /MP
                $<$<CONFIG:Debug>:/Od>
                $<$<CONFIG:Release>:/O2>
        )
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${TARGET_NAME}
            PRIVATE
                -Wall -Wextra
                $<$<CONFIG:Debug>:-O0 -g>
                $<$<CONFIG:Release>:-O2>
        )
    endif()
endmacro()


# ── LOOPTests_Roundtrip ───────────────────────────────────────────────────────
# Exercises LoopFileWriter + LoopFileReader (Phase 1a + 1b).
# Does NOT require audio hardware.
add_executable(LOOPTests_Roundtrip
    tests/test_format_roundtrip.cpp
    src/LoopFileWriter.cpp
    src/LoopFileReader.cpp
)
loop_test_target(LOOPTests_Roundtrip)
target_link_libraries(LOOPTests_Roundtrip
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)


# ── LOOPTests_SequencerTiming ─────────────────────────────────────────────────
# Exercises LoopSequencer + timing math (Phase 2).
# Drives getNextAudioBlock() with a fake buffer — no audio hardware needed.
add_executable(LOOPTests_SequencerTiming
    tests/test_sequencer_timing.cpp
    src/LoopFileWriter.cpp
    src/LoopFileReader.cpp
    src/LoopSequencer.cpp
)
loop_test_target(LOOPTests_SequencerTiming)
target_link_libraries(LOOPTests_SequencerTiming
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)


# ── LOOPTests_ZeroCrossing ────────────────────────────────────────────────────
# Exercises findZeroCrossing_16() and LoopFileWriter trim integration.
add_executable(LOOPTests_ZeroCrossing
    tests/test_zero_crossing.cpp
    src/LoopFileWriter.cpp
    src/LoopFileReader.cpp
)
loop_test_target(LOOPTests_ZeroCrossing)
target_link_libraries(LOOPTests_ZeroCrossing
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)


# ── LOOPCompiler ─────────────────────────────────────────────────────────────
# Command-line tool to compile WAV files and a JSON config into a .LOOP file.
add_executable(LOOPCompiler
    src/LoopCompiler.cpp
    src/LoopFileWriter.cpp
    src/LoopFileReader.cpp
)
loop_test_target(LOOPCompiler)
target_link_libraries(LOOPCompiler
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)


# ── CTest integration (optional) ──────────────────────────────────────────────
enable_testing()
add_test(NAME RoundtripTests      COMMAND LOOPTests_Roundtrip)
add_test(NAME SequencerTimingTests COMMAND LOOPTests_SequencerTiming)
add_test(NAME ZeroCrossingTests   COMMAND LOOPTests_ZeroCrossing)


# ── Install / package ─────────────────────────────────────────────────────────
# Uncomment to add install targets for packaging:
# install(TARGETS LOOPPlayer_VST3 DESTINATION "VST3")
