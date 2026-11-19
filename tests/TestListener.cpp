#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <juce_events/juce_events.h>

namespace
{
/** Tears down JUCE singletons created by tests (the MessageManager), so leak checks stay quiet. */
class JuceShutdownListener final : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats&) override
    {
        juce::DeletedAtShutdown::deleteAll();
        juce::MessageManager::deleteInstance();
    }
};
} // namespace

CATCH_REGISTER_LISTENER(JuceShutdownListener)
