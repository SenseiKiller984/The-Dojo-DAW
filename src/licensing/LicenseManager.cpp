#include "licensing/LicenseManager.h"

#include "licensing/PublicKey.h"

#include <cmath>

namespace dojo::licensing
{

LicenseManager::LicenseManager(Config config)
    : config_(std::move(config))
    , ledger_(std::move(config_.ledgerStores), config_.machineId)
{
    if (!config_.clock)
        config_.clock = [] { return juce::Time::currentTimeMillis(); };
    apply(evaluate());
}

LicenseManager::~LicenseManager() = default;

int LicenseManager::getTrialDaysLeft() const
{
    if (status_.tier != Tier::demo)
        return 0;
    const auto left = status_.trialEndsMs - config_.clock();
    return left <= 0 ? 0 : static_cast<int>(std::ceil(static_cast<double>(left) / (24.0 * 60 * 60 * 1000)));
}

LicenseManager::Status LicenseManager::evaluate()
{
    Status status;
    const auto now = config_.clock();
    const auto stored = config_.keyStore != nullptr ? config_.keyStore->read() : std::nullopt;

    if (!stored.has_value() || stored->trim().isEmpty())
    {
        const auto trial = ledger_.evaluate(now); // (keeps the clock record, when there is one)
        status.message = trial.state == DemoLedger::State::unused   ? "Enter your license key to start The Dojo."
                         : trial.state == DemoLedger::State::active ? "The free trial is still running: enter its key again to go on, or a Basic or Pro key."
                                                                    : "The free trial on this computer has ended. Enter a Basic or Pro license key to keep using The Dojo.";
        return status;
    }

    juce::String error;
    const auto payload = verifyKey(*stored, config_.publicKey, error);
    if (!payload.has_value())
    {
        status.message = "The saved license key does not work: " + error;
        return status;
    }
    if (isRevoked(*payload))
    {
        status.message = "This license key has been revoked. Enter another license key.";
        return status;
    }

    if (payload->tier == Tier::demo)
    {
        auto trial = ledger_.evaluate(now);
        if (trial.state == DemoLedger::State::unused)
        {
            // A trial key with no record of its trial: the record was removed. It counts as used.
            (void)ledger_.start(now - DemoLedger::kTrialMs, payload->serial);
            trial = ledger_.evaluate(now);
            trial.reason = "The free trial's record on this computer was removed.";
        }
        if (trial.state != DemoLedger::State::active)
        {
            status.message = trial.reason + " Enter a Basic or Pro license key to keep using The Dojo.";
            return status;
        }
        status.tier = Tier::demo;
        status.serial = payload->serial;
        status.trialEndsMs = trial.endsMs;
        status.message = "Free trial: exporting is off.";
        return status;
    }

    status.tier = payload->tier;
    status.serial = payload->serial;
    status.message = getTierName(payload->tier) + " license (serial " + juce::String(payload->serial) + ").";
    return status;
}

void LicenseManager::apply(Status status)
{
    const bool changed = status.tier != status_.tier || status.message != status_.message || status.trialEndsMs != status_.trialEndsMs;
    status_ = std::move(status);
    if (config_.publishToGate)
        gate::publish(status_.tier);
    if (changed)
        sendChangeMessage();
}

LicenseManager::Activation LicenseManager::activate(const juce::String& keyText)
{
    Activation result;
    juce::String error;
    const auto payload = verifyKey(keyText, config_.publicKey, error);
    if (!payload.has_value())
    {
        result.message = error;
        return result;
    }
    if (isRevoked(*payload))
    {
        result.message = "This license key has been revoked.";
        return result;
    }

    if (payload->tier == Tier::demo)
    {
        if (status_.tier == Tier::basic || status_.tier == Tier::pro)
        {
            result.message = "You have a " + getTierName(status_.tier) + " license already: the free trial would only take features away.";
            return result;
        }
        if (status_.tier == Tier::demo)
        {
            result.message = "The free trial is running already.";
            return result;
        }
        const auto now = config_.clock();
        // A trial still running (its key was removed) goes on where it was; it never starts twice.
        if (ledger_.evaluate(now).state != DemoLedger::State::active)
        {
            switch (ledger_.start(now, payload->serial))
            {
            case DemoLedger::StartResult::started:
                break;
            case DemoLedger::StartResult::alreadyUsed:
                result.message = "The free trial has already been used on this computer: it works only once. Enter a Basic or Pro license key.";
                return result;
            case DemoLedger::StartResult::notRecorded:
                result.message = "The free trial could not be recorded on this computer (The Dojo cannot write to its settings folders), so it "
                                 "cannot start.";
                return result;
            }
        }
    }

    if (config_.keyStore != nullptr)
        config_.keyStore->write(normaliseKey(keyText));
    apply(evaluate());

    result.ok = status_.tier == payload->tier;
    result.message = result.ok ? (payload->tier == Tier::demo ? juce::String("Your 7-day free trial has started.")
                                                              : getTierName(payload->tier) + " license activated. Thank you!")
                               : status_.message;
    return result;
}

void LicenseManager::removeKey()
{
    if (config_.keyStore != nullptr)
        config_.keyStore->write({});
    apply(evaluate());
}

void LicenseManager::refresh()
{
    apply(evaluate());
}

juce::String LicenseManager::getMachineId()
{
    auto id = juce::SystemStats::getUniqueDeviceID();
    if (id.isEmpty())
        id = juce::SystemStats::getComputerName() + "/" + juce::SystemStats::getLogonName();
    return id;
}

std::unique_ptr<LicenseManager> LicenseManager::createForApplication(juce::PropertiesFile& settings)
{
    Config config;
    config.publicKey = getProductionPublicKey();
    config.keyStore = std::make_unique<PropertiesMarkerStore>(settings, kKeySetting);
    config.machineId = getMachineId();
    config.publishToGate = true;
    // The time: never earlier than the file system's (a program faking the clock to The Dojo alone,
    // LD_PRELOAD-style, does not reach the kernel's file times).
    config.clock = [tick = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                               .getChildFile(".dojo-state")
                               .getChildFile("tick")]
    {
        const auto system = juce::Time::currentTimeMillis();
        if (tick.getParentDirectory().createDirectory() && tick.replaceWithText(juce::String(system)))
            return std::max(system, tick.getLastModificationTime().toMilliseconds());
        return system;
    };

    // The trial's record, in several places (see DemoLedger).
    auto& stores = config.ledgerStores;
    stores.push_back(std::make_unique<PropertiesMarkerStore>(settings, "layoutCache"));
    const auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    stores.push_back(std::make_unique<FileMarkerStore>(appData.getChildFile(".dojo-state").getChildFile("ls.dat")));
#if JUCE_WINDOWS
    stores.push_back(std::make_unique<FileMarkerStore>(
        juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("TheDojo").getChildFile(".cache").getChildFile("fs.dat")));
    stores.push_back(std::make_unique<RegistryMarkerStore>("HKEY_CURRENT_USER\\Software\\TheDojo\\Cache\\State"));
#else
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    stores.push_back(std::make_unique<FileMarkerStore>(home.getChildFile(".local").getChildFile("share").getChildFile(".dojo").getChildFile("fs.dat")));
#endif
    stores.push_back(std::make_unique<FileMarkerStore>(
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".dojo-rt")));

    return std::make_unique<LicenseManager>(std::move(config));
}

} // namespace dojo::licensing
