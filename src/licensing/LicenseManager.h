#pragma once

#include "licensing/DemoLedger.h"
#include "licensing/Features.h"
#include "licensing/LicenseKey.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>

/**
    The license The Dojo runs under: the key the user entered (kept in the settings), checked with
    the embedded public key every time it is read, and, for the free trial, the ledger.

    Nothing is trusted from storage but the key's text: the tier comes from its signature each time,
    so editing the settings cannot change it. A trial key works once per computer, for seven days
    from its activation.

    Message thread only.
*/
namespace dojo::licensing
{

class LicenseManager final : public juce::ChangeBroadcaster
{
public:
    struct Config
    {
        crypto::PublicKey publicKey{};
        std::unique_ptr<MarkerStore> keyStore;                   ///< where the entered key is kept
        std::vector<std::unique_ptr<MarkerStore>> ledgerStores;  ///< where the trial's record is kept
        juce::String machineId;
        std::function<std::int64_t()> clock;                     ///< UTC milliseconds (default: the system clock)
        bool publishToGate = false;                              ///< the application's manager: sets gate::publish
    };

    struct Status
    {
        Tier tier = Tier::none;
        std::uint32_t serial = 0;
        std::int64_t trialEndsMs = 0; ///< the free trial's end (when tier is demo)
        juce::String message;         ///< for the license window: why there is no license, or what there is
    };

    struct Activation
    {
        bool ok = false;
        juce::String message;
    };

    explicit LicenseManager(Config config);
    ~LicenseManager() override;

    [[nodiscard]] const Status& getStatus() const noexcept { return status_; }
    [[nodiscard]] Tier getTier() const noexcept { return status_.tier; }
    [[nodiscard]] bool allows(Feature feature) const noexcept { return has(featuresOf(status_.tier), feature); }
    /** Whole days of the trial left (rounded up); 0 outside the trial. */
    [[nodiscard]] int getTrialDaysLeft() const;

    /** Checks and keeps a key. A trial key starts this computer's one free trial. */
    Activation activate(const juce::String& keyText);
    /** Forgets the entered key (the trial stays used). */
    void removeKey();
    /** Looks again (the trial's end, the clock): call now and then. Broadcasts when the status changed. */
    void refresh();

    /** The application's manager: the production public key, the settings and the ledger's places. */
    [[nodiscard]] static std::unique_ptr<LicenseManager> createForApplication(juce::PropertiesFile& settings);
    /** An id of this computer (stable across runs; for binding the trial's record). */
    [[nodiscard]] static juce::String getMachineId();
    /** The settings key the entered key is kept under. */
    static constexpr const char* kKeySetting = "licenseKey";

private:
    Status evaluate();
    void apply(Status status);

    Config config_;
    DemoLedger ledger_;
    Status status_;
};

} // namespace dojo::licensing
