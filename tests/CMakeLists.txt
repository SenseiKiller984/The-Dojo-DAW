FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(dojo_tests
    DeviceTestSupport.h
    TestAudio.h
    TestListener.cpp
    TestSupport.h
    AudioEngineTests.cpp
    BrowserTests.cpp
    ClickTests.cpp
    ClipListTests.cpp
    ClipLockTests.cpp
    ClipMenuTests.cpp
    ModulationEffectTests.cpp
    MultichannelTests.cpp
    TestVideo.h
    VideoMuxTests.cpp
    VideoTests.cpp
    DeviceEditorTests.cpp
    DeviceSendTests.cpp
    DeviceTests.cpp
    DriveTests.cpp
    DynamicsFilterTests.cpp
    EffectTestHelpers.h
    EditorDropTests.cpp
    EffectTests.cpp
    EngineGraphTests.cpp
    InstrumentTests.cpp
    LatencyDisplayTests.cpp
    LfoTests.cpp
    LimiterTests.cpp
    MidiEffectTests.cpp
    PitchEditTests.cpp
    PluginTests.cpp
    PreferencesTests.cpp
    PresetChainTests.cpp
    PresetTests.cpp
    PreviewTests.cpp
    RecordExportTests.cpp
    EngineCommandTests.cpp
    MusicalTimeTests.cpp
    RenderGraphTests.cpp
    ScoreTests.cpp
    SelectionTests.cpp
    TrackMenuTests.cpp
    TrackSelectionTests.cpp
    SessionTests.cpp
    RealtimeGuardTests.cpp
    SpectralEditTests.cpp
    SpectrumOverlayTests.cpp
    HardwareInsertTests.cpp
    ControlSurfaceTests.cpp
    WarpTests.cpp
    TakeLaneTests.cpp
    NoteExpressionTests.cpp
    HeightBinauralTests.cpp
    AdmTests.cpp
    AdmExportTests.cpp
    AdmOscTests.cpp
    AraTests.cpp
    ImmersiveBedTests.cpp
    SpatialPannerTests.cpp
    SpatialPannerUiTests.cpp
    SpscQueueTests.cpp
    StudioDeviceTests.cpp
    ConsoleLookTests.cpp
    ConsoleTests.cpp
    HarrisonTests.cpp
    InsertLoopTests.cpp
    RhodesTests.cpp
    TapeDeckTests.cpp
    TapeTests.cpp
    SurroundTests.cpp
    SynthTests.cpp
    TempoTests.cpp
    TempoMapTests.cpp
    ValueEntryTests.cpp
    WaveformTests.cpp)

# The suite runs with the allocation trap so that RT-safety is asserted, not assumed
# (except in ASan/TSan builds, whose runtimes own operator new/delete).
if(DOJO_ALLOCATION_TRAP_COMPATIBLE)
    target_sources(dojo_tests PRIVATE $<TARGET_OBJECTS:dojo_allocation_trap>)
endif()
target_compile_definitions(dojo_tests PRIVATE
    DOJO_EXPECT_ALLOCATION_TRAP=$<BOOL:${DOJO_ALLOCATION_TRAP_COMPATIBLE}>)

target_link_libraries(dojo_tests PRIVATE dojo::ui dojo::session dojo::plugins dojo::video Catch2::Catch2WithMain)

# Windows: H.264 test movies made (and their sound read back) with Media Foundation, for the muxer.
if(WIN32)
    target_sources(dojo_tests PRIVATE MediaFoundationTestMovie.cpp MediaFoundationTestMovie.h)
    target_link_libraries(dojo_tests PRIVATE mfplat mfreadwrite mfuuid ole32)
endif()

# A real CLAP binary (two plug-ins) for the hosting tests, and the out-of-process scanner.
add_library(dojo_test_clap MODULE clap/DojoTestPlugins.cpp)
target_link_libraries(dojo_test_clap PRIVATE dojo::clap)
set_target_properties(dojo_test_clap PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    OUTPUT_NAME "DojoTestPlugins"
    CXX_VISIBILITY_PRESET hidden
    FOLDER "Tests")
add_dependencies(dojo_tests dojo_test_clap dojo-plugin-scanner)
target_compile_definitions(dojo_tests PRIVATE
    DOJO_TEST_CLAP_PATH="$<TARGET_FILE:dojo_test_clap>"
    DOJO_SCANNER_PATH="$<TARGET_FILE:dojo-plugin-scanner>")
set_target_properties(dojo_tests PROPERTIES FOLDER "Tests")

# A small ARA 2 plug-in (VST3, built with JUCE's plug-in client and its own copy of JUCE) for the
# ARA hosting tests: it reads its regions' audio through the host, archives what it measured and
# renders the regions. Test fixture code, compiled quietly like JUCE itself.
if(DOJO_WITH_ARA)
    juce_add_plugin(dojo_test_ara
        PRODUCT_NAME "DojoTestAra"
        COMPANY_NAME "The Dojo Tests"
        BUNDLE_ID "dev.thedojo.tests.ara"
        PLUGIN_MANUFACTURER_CODE Dojo
        PLUGIN_CODE DjAr
        FORMATS VST3
        IS_ARA_EFFECT TRUE
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        VST3_AUTO_MANIFEST FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE)
    target_sources(dojo_test_ara PRIVATE ara/DojoTestAra.cpp)
    target_compile_definitions(dojo_test_ara PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(dojo_test_ara PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)
    if(MSVC)
        target_compile_options(dojo_test_ara PRIVATE /W0 /bigobj)
    else()
        target_compile_options(dojo_test_ara PRIVATE -w)
    endif()
    set_target_properties(dojo_test_ara dojo_test_ara_VST3 PROPERTIES FOLDER "Tests")
    add_dependencies(dojo_tests dojo_test_ara_VST3)
    get_target_property(dojo_test_ara_bundle dojo_test_ara_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
    target_compile_definitions(dojo_tests PRIVATE DOJO_TEST_ARA_PATH="${dojo_test_ara_bundle}")
endif()

catch_discover_tests(dojo_tests DISCOVERY_MODE PRE_TEST)
