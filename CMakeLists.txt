cmake_minimum_required(VERSION 3.22)

project(TheDojo
    VERSION 0.1.0
    DESCRIPTION "The Dojo - a cross-platform digital audio workstation"
    HOMEPAGE_URL "https://github.com/SenseiKiller984/The-Dojo-DAW"
    LANGUAGES C CXX)

if(NOT (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(WARNING
        "The Dojo officially targets Windows and Linux only. "
        "Building for '${CMAKE_SYSTEM_NAME}' is untested and unsupported.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(DojoOptions)
include(DojoCompilerSettings)
include(DojoDependencies)

add_subdirectory(resources)
add_subdirectory(src)

if(DOJO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "The Dojo ${PROJECT_VERSION}")
message(STATUS "  Platform ............ ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  Compiler ............ ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  JUCE ................ ${DOJO_JUCE_VERSION}")
message(STATUS "  Tests ............... ${DOJO_BUILD_TESTS}")
message(STATUS "  RT allocation trap .. ${DOJO_RT_ALLOCATION_TRAP}")
message(STATUS "  Sanitizer ........... ${DOJO_SANITIZER}")
message(STATUS "")
