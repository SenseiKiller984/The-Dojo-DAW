# User-facing build options for The Dojo.

option(DOJO_BUILD_TESTS "Build the unit test suite (fetches Catch2)" ON)

option(DOJO_WARNINGS_AS_ERRORS "Treat compiler warnings in Dojo sources as errors" OFF)

option(DOJO_ENABLE_LTO "Enable link-time optimisation in optimised configurations" OFF)

option(DOJO_RT_ALLOCATION_TRAP
    "Link the real-time allocation trap into the app (all configurations except Release). \
The trap replaces global operator new/delete and counts any heap traffic that happens \
inside a dojo::rt::ScopedRealtimeSection, i.e. on the audio thread."
    ON)

# Audio back-ends. Each is only meaningful on its own platform; the flags are ignored elsewhere.
option(DOJO_ENABLE_ASIO "Windows: enable the Steinberg ASIO back-end (SDK bundled with JUCE, GPLv3)" ON)
option(DOJO_ENABLE_JACK "Linux: enable the JACK back-end (also used by PipeWire's JACK API)" ON)

set(DOJO_SANITIZER "" CACHE STRING "GCC/Clang sanitizer to instrument Dojo targets with: (empty), address, undefined, thread")
set_property(CACHE DOJO_SANITIZER PROPERTY STRINGS "" address undefined thread)
