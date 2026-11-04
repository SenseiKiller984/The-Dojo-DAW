FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(dojo_tests
    TestSupport.h
    AudioEngineTests.cpp
    EngineCommandTests.cpp
    MusicalTimeTests.cpp
    RealtimeGuardTests.cpp
    SpscQueueTests.cpp)

# The suite runs with the allocation trap so that RT-safety is asserted, not assumed
# (except in ASan/TSan builds, whose runtimes own operator new/delete).
if(DOJO_ALLOCATION_TRAP_COMPATIBLE)
    target_sources(dojo_tests PRIVATE $<TARGET_OBJECTS:dojo_allocation_trap>)
endif()
target_compile_definitions(dojo_tests PRIVATE
    DOJO_EXPECT_ALLOCATION_TRAP=$<BOOL:${DOJO_ALLOCATION_TRAP_COMPATIBLE}>)

target_link_libraries(dojo_tests PRIVATE dojo::engine Catch2::Catch2WithMain)
set_target_properties(dojo_tests PROPERTIES FOLDER "Tests")

catch_discover_tests(dojo_tests DISCOVERY_MODE PRE_TEST)
