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
    VideoTests.cpp
    DeviceEditorTests.cpp
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
    PluginTests.cpp
    PreferencesTests.cpp
    PresetChainTests.cpp
    PresetTests.cpp
    PreviewTests.cpp
    RecordExportTests.cpp
    EngineCommandTests.cpp
    MusicalTimeTests.cpp
    RenderGraphTests.cpp
    SelectionTests.cpp
    TrackMenuTests.cpp
    TrackSelectionTests.cpp
    SessionTests.cpp
    RealtimeGuardTests.cpp
    SpscQueueTests.cpp
    StudioDeviceTests.cpp
    SurroundTests.cpp
    SynthTests.cpp
    TempoTests.cpp
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

catch_discover_tests(dojo_tests DISCOVERY_MODE PRE_TEST)
