#pragma once

#include "core/rt/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

// DOJO_EXPECT_ALLOCATION_TRAP is set by tests/CMakeLists.txt from the build configuration, so a
// normal build in which the trap failed to link FAILS, while ASan/TSan builds (whose runtimes own
// operator new/delete and therefore cannot carry the trap) report the dependent tests as SKIPPED.
#if DOJO_EXPECT_ALLOCATION_TRAP
#define DOJO_REQUIRE_ALLOCATION_TRAP() REQUIRE(::dojo::rt::isAllocationTrapInstalled())
#else
#define DOJO_REQUIRE_ALLOCATION_TRAP()                                                                       \
    SKIP("allocation trap not linked (ASan/TSan build: the sanitizer owns operator new/delete)")
#endif
