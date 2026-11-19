#include "engine/EngineCommands.h"

#include <catch2/catch_test_macros.hpp>

using namespace dojo::engine;

TEST_CASE("Commands of the same coalescable type can be merged", "[engine][commands]")
{
    const EngineCommand a = command::SetMasterGain{0.5};
    const EngineCommand b = command::SetMasterGain{0.25};
    CHECK(canCoalesce(a, b));
}

TEST_CASE("Commands of different types are never merged", "[engine][commands]")
{
    const EngineCommand gain = command::SetMasterGain{0.5};
    const EngineCommand play = command::SetPlaying{true};
    const EngineCommand locate = command::Locate{48'000};

    CHECK_FALSE(canCoalesce(gain, play));
    CHECK_FALSE(canCoalesce(play, locate));
    CHECK_FALSE(canCoalesce(locate, gain));
}
