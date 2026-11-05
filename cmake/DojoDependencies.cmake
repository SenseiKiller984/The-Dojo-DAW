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
        # Audio back-ends, each enabled only on its own platform. (JUCE can also build JACK on
        # Windows, which would need the JACK SDK headers there; The Dojo uses ASIO/WASAPI instead.)
        JUCE_ASIO=$<AND:$<BOOL:${DOJO_ENABLE_ASIO}>,$<PLATFORM_ID:Windows>>
        JUCE_WASAPI=$<PLATFORM_ID:Windows>
        JUCE_DIRECTSOUND=$<PLATFORM_ID:Windows>
        JUCE_ALSA=$<PLATFORM_ID:Linux>
        JUCE_JACK=$<AND:$<BOOL:${DOJO_ENABLE_JACK}>,$<PLATFORM_ID:Linux>>
        # Plug-in hosting is switched on in the plug-in hosting phase (VST3 + CLAP on both OSes).
        # LV2/LADSPA/AU are deliberately never enabled: they are not available on BOTH Windows and
        # Linux, and The Dojo guarantees feature parity across the two.
        JUCE_PLUGINHOST_VST3=0
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
