find_package(Threads REQUIRED)

set(_dojo_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(Version.h.in "${_dojo_generated_dir}/core/Version.h" @ONLY)

add_library(dojo_core STATIC
    MusicalTime.h
    Types.h
    Version.h.in
    "${_dojo_generated_dir}/core/Version.h"
    rt/AtomicUtils.h
    rt/RealtimeGuard.cpp
    rt/RealtimeGuard.h
    rt/SpscQueue.h
    rt/ThreadOwnershipChecker.h)
add_library(dojo::core ALIAS dojo_core)

target_include_directories(dojo_core PUBLIC ${PROJECT_SOURCE_DIR}/src "${_dojo_generated_dir}")
target_compile_features(dojo_core PUBLIC cxx_std_20)
target_link_libraries(dojo_core PUBLIC Threads::Threads dojo::sanitizers)
dojo_apply_project_warnings(dojo_core)
set_target_properties(dojo_core PROPERTIES FOLDER "Dojo")

# --------------------------------------------------------------------------------------------------
# The real-time allocation trap replaces the global operator new/delete family. Replacement
# allocation functions must be linked directly into the final executable (not pulled from a static
# archive, where the linker may legitimately skip them), so this is an OBJECT library whose objects
# are added to executables explicitly — see src/app and tests.
#
# ASan/TSan runtimes install their own operator new/delete, so the trap cannot be combined with them;
# sanitizer builds omit it (the trap-dependent tests then report SKIPPED).
if(DOJO_SANITIZER MATCHES "^(address|thread)$")
    set(DOJO_ALLOCATION_TRAP_COMPATIBLE OFF CACHE INTERNAL "")
else()
    set(DOJO_ALLOCATION_TRAP_COMPATIBLE ON CACHE INTERNAL "")
endif()

add_library(dojo_allocation_trap OBJECT rt/AllocationTrap.cpp)
target_link_libraries(dojo_allocation_trap PRIVATE dojo::core)
dojo_apply_project_warnings(dojo_allocation_trap)
set_target_properties(dojo_allocation_trap PROPERTIES FOLDER "Dojo")
