# Third-party dependencies, pinned to exact versions.
#
# Offline builds / local checkouts: point FetchContent at an existing source tree, e.g.
#   cmake --preset linux-release -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE

include(FetchContent)

# --------------------------------------------------------------------------------------------------
# JUCE — audio device I/O (ASIO/WASAPI/DirectSound, ALSA/JACK), MIDI I/O, plug-in hosting,
# audio file formats, DSP primitives and the GUI toolkit.
set(DOJO_JUCE_VERSION "9.0.4")   # tag commit 3fb5cbc0340758596dac39f7c8de47d562b4896e

FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${DOJO_JUCE_VERSION}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(juce)

# --------------------------------------------------------------------------------------------------
# dojo::juce — every JUCE module The Dojo uses, compiled exactly ONCE into a static library and then
# shared by the app, the engine and the test-suite. (Linking juce::juce_* INTERFACE targets directly
# into several targets would recompile the modules for each of them.)
add_library(dojo_juce STATIC)
add_library(dojo::juce ALIAS dojo_juce)

target_link_libraries(dojo_juce
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        dojo::sanitizers)

target_compile_definitions(dojo_juce
    PUBLIC
        JUCE_STANDALONE_APPLICATION=1
        JUCE_DISABLE_JUCE_VERSION_PRINTING=1
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        # Lets headless tests pump the message loop (MessageManager::runDispatchLoopUntil). The UI
        # itself only uses asynchronous dialogs.
        JUCE_MODAL_LOOPS_PERMITTED=1
        # Audio back-ends, each enabled only on its own platform. (JUCE can also build JACK on
        # Windows, which would need the JACK SDK headers there; The Dojo uses ASIO/WASAPI instead.)
        JUCE_ASIO=$<AND:$<BOOL:${DOJO_ENABLE_ASIO}>,$<PLATFORM_ID:Windows>>
        JUCE_WASAPI=$<PLATFORM_ID:Windows>
        JUCE_DIRECTSOUND=$<PLATFORM_ID:Windows>
        JUCE_ALSA=$<PLATFORM_ID:Linux>
        JUCE_JACK=$<AND:$<BOOL:${DOJO_ENABLE_JACK}>,$<PLATFORM_ID:Linux>>
        # Plug-in hosting: VST3 through JUCE, CLAP through Dojo's own host (src/plugins) on the official
        # CLAP SDK. LV2/LADSPA/AU are deliberately never enabled: they are not available on BOTH
        # Windows and Linux, and The Dojo guarantees feature parity across the two.
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_AU=0)

# Re-export the module include paths and the (transitively evaluated) module definitions, so that
# consumers see exactly the same JUCE configuration as the TUs that compiled the modules.
target_include_directories(dojo_juce INTERFACE $<TARGET_PROPERTY:dojo_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(dojo_juce INTERFACE $<TARGET_PROPERTY:dojo_juce,COMPILE_DEFINITIONS>)

set_target_properties(dojo_juce PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "ThirdParty")

# JUCE and the third-party codecs it bundles (Opus, FLAC, Ogg Vorbis, HarfBuzz, ...) are not our code:
# new compiler releases regularly add diagnostics that fire inside them (e.g. GCC 14's
# -Wstringop-overflow false positives in Opus' SILK decoder). Those warnings are not actionable here,
# so the vendored sources are compiled quietly, like system headers. Dojo's own targets keep the
# strict warning set (see dojo_apply_project_warnings).
if(MSVC)
    target_compile_options(dojo_juce PRIVATE /bigobj /MP /W0)
else()
    target_compile_options(dojo_juce PRIVATE -w)
endif()

# --------------------------------------------------------------------------------------------------
# CLAP SDK (MIT) — headers only, for Dojo's CLAP plug-in host.
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        1.2.10
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  dojo-headers-only) # nothing to add_subdirectory(): we only need include/
FetchContent_MakeAvailable(clap)
add_library(dojo_clap INTERFACE)
add_library(dojo::clap ALIAS dojo_clap)
target_include_directories(dojo_clap SYSTEM INTERFACE "${clap_SOURCE_DIR}/include")

# --------------------------------------------------------------------------------------------------
# Signalsmith Stretch (MIT) — polyphonic time-stretch and pitch-shift, for clip stretching and the
# Speed & Pitch device. Header-only, plus its header-only FFT/STFT library.
FetchContent_Declare(signalsmith_linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG        0.6.4
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  dojo-headers-only)
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        1.4.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  dojo-headers-only)
FetchContent_MakeAvailable(signalsmith_linear signalsmith_stretch)
add_library(dojo_stretch INTERFACE)
add_library(dojo::stretch ALIAS dojo_stretch)
target_include_directories(dojo_stretch SYSTEM INTERFACE
    "${signalsmith_stretch_SOURCE_DIR}/include"
    "${signalsmith_linear_SOURCE_DIR}/include")

# --------------------------------------------------------------------------------------------------
# LAME (LGPL) MP3 encoder, vendored under third_party/lame.
add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/lame" "${CMAKE_BINARY_DIR}/third_party/lame")
