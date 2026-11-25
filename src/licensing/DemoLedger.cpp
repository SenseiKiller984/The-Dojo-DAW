#include "licensing/DemoLedger.h"

#include "licensing/Crypto.h"

#include <algorithm>

namespace dojo::licensing
{
namespace
{
constexpr const char* kFormat = "L1";

juce::String toHex(std::span<const std::uint8_t> bytes)
{
    return juce::String::toHexString(bytes.data(), static_cast<int>(bytes.size()), 0);
}

std::span<const std::uint8_t> bytesOf(const juce::String& text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.toRawUTF8()), text.getNumBytesAsUTF8()};
}

/** The record's fields as text (what the seal covers). */
juce::String fieldsOf(const DemoLedger::Record& record)
{
    return juce::String(kFormat) + "|" + juce::String(record.startedMs) + "|" + juce::String(record.lastSeenMs) + "|"
           + juce::String(static_cast<juce::int64>(record.serial)) + "|" + (record.ended ? "1" : "0");
}
} // namespace

//==================================================================================================
// Stores

std::optional<juce::String> FileMarkerStore::read() const
{
    if (!file_.existsAsFile())
        return std::nullopt;
    return file_.loadFileAsString().trim();
}

std::optional<std::int64_t> FileMarkerStore::lastWrittenMs() const
{
    if (!file_.existsAsFile())
        return std::nullopt;
    return file_.getLastModificationTime().toMilliseconds();
}

void FileMarkerStore::write(const juce::String& text)
{
    if (!file_.getParentDirectory().createDirectory())
        return;
    if (file_.existsAsFile() && file_.loadFileAsString().trim() == text)
        return;
    (void)file_.replaceWithText(text);
}

std::optional<juce::String> PropertiesMarkerStore::read() const
{
    if (!properties_.containsKey(key_))
        return std::nullopt;
    return properties_.getValue(key_).trim();
}

void PropertiesMarkerStore::write(const juce::String& text)
{
    if (properties_.getValue(key_) == text)
        return;
    properties_.setValue(key_, text);
    (void)properties_.saveIfNeeded();
}

std::optional<juce::String> RegistryMarkerStore::read() const
{
#if JUCE_WINDOWS
    if (!juce::WindowsRegistry::valueExists(valuePath_))
        return std::nullopt;
    return juce::WindowsRegistry::getValue(valuePath_).trim();
#else
    return std::nullopt;
#endif
}

void RegistryMarkerStore::write([[maybe_unused]] const juce::String& text)
{
#if JUCE_WINDOWS
    if (juce::WindowsRegistry::valueExists(valuePath_) && juce::WindowsRegistry::getValue(valuePath_) == text)
        return;
    (void)juce::WindowsRegistry::setValue(valuePath_, text);
#endif
}

//==================================================================================================
// Ledger

DemoLedger::DemoLedger(std::vector<std::unique_ptr<MarkerStore>> stores, juce::String machineId)
    : stores_(std::move(stores))
{
    // The seal key: a hash of this computer's id under a fixed key (so a record copied from another
    // computer does not unseal here).
    static constexpr std::uint8_t kLedgerKey[] = {0x5d, 0x0a, 0xe3, 0x19, 0x7c, 0x42, 0xb8, 0x91, 0x2f, 0x66, 0xd4,
                                                  0x0b, 0x93, 0x58, 0xc1, 0x7e, 0x24, 0xaf, 0x60, 0x3d, 0xe9, 0x15,
                                                  0x8a, 0x47, 0xbc, 0x02, 0x71, 0xd6, 0x39, 0x84, 0xfe, 0x5b};
    const juce::String material = "TheDojo/ledger/v1|" + machineId;
    crypto::keyedHash(sealKey_, kLedgerKey, bytesOf(material));
}

juce::String DemoLedger::seal(const Record& record) const
{
    const auto fields = fieldsOf(record);
    std::array<std::uint8_t, 16> tag{};
    crypto::keyedHash(tag, sealKey_, bytesOf(fields));
    const auto text = fields + "|" + toHex(tag);
    return juce::Base64::toBase64(text);
}

std::optional<DemoLedger::Record> DemoLedger::unseal(const juce::String& sealed) const
{
    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, sealed.trim()))
        return std::nullopt;
    const auto text = decoded.toString();
    auto parts = juce::StringArray::fromTokens(text, "|", "");
    if (parts.size() != 6 || parts[0] != kFormat)
        return std::nullopt;
    Record record;
    record.startedMs = parts[1].getLargeIntValue();
    record.lastSeenMs = parts[2].getLargeIntValue();
    record.serial = static_cast<std::uint32_t>(parts[3].getLargeIntValue());
    record.ended = parts[4] == "1";
    if (fieldsOf(record) + "|" + parts[5] != text) // (one text per record: no leading zeros, spaces...)
        return std::nullopt;

    std::array<std::uint8_t, 16> expected{};
    crypto::keyedHash(expected, sealKey_, bytesOf(fieldsOf(record)));
    juce::MemoryBlock given;
    given.loadFromHexString(parts[5]);
    if (given.getSize() != expected.size()
        || !crypto::equal(expected, {static_cast<const std::uint8_t*>(given.getData()), given.getSize()}))
        return std::nullopt;
    return record;
}

DemoLedger::Merged DemoLedger::readAll() const
{
    Merged merged;
    for (const auto& store : stores_)
    {
        const auto text = store->read();
        if (!text.has_value())
            continue;
        merged.present = true;
        const auto record = unseal(*text);
        if (!record.has_value())
        {
            merged.tampered = true; // edited, garbled, or copied from another computer
            continue;
        }
        if (merged.record.startedMs == 0 || (record->startedMs > 0 && record->startedMs < merged.record.startedMs))
        {
            merged.record.startedMs = record->startedMs;
            merged.record.serial = record->serial;
        }
        merged.record.lastSeenMs = std::max(merged.record.lastSeenMs, record->lastSeenMs);
        merged.record.ended = merged.record.ended || record->ended;
    }
    return merged;
}

void DemoLedger::writeAll(const Record& record)
{
    const auto text = seal(record);
    for (auto& store : stores_)
        store->write(text);
}

DemoLedger::Status DemoLedger::evaluate(std::int64_t nowMs)
{
    Status status;
    auto merged = readAll();
    if (merged.present)
        for (const auto& store : stores_)
            if (const auto written = store->lastWrittenMs())
                nowMs = std::max(nowMs, *written);
    if (!merged.present)
        return status; // unused

    auto& record = merged.record;
    if (merged.tampered && !record.ended)
    {
        record.ended = true;
        status.reason = "The free trial's record on this computer was changed.";
    }
    if (record.startedMs <= 0)
    {
        record.ended = true;
        if (status.reason.isEmpty())
            status.reason = "The free trial's record on this computer was changed.";
    }
    if (!record.ended && nowMs + kRollbackToleranceMs < record.lastSeenMs)
    {
        record.ended = true;
        status.reason = "The computer's clock was turned back during the free trial.";
    }
    if (!record.ended && nowMs >= record.startedMs + kTrialMs)
    {
        record.ended = true;
        status.reason = "The 7-day free trial has ended.";
    }
    if (record.ended && status.reason.isEmpty())
        status.reason = "The free trial on this computer has ended.";

    record.lastSeenMs = std::max(record.lastSeenMs, nowMs);
    writeAll(record);

    status.state = record.ended ? State::ended : State::active;
    status.startedMs = record.startedMs;
    status.endsMs = record.startedMs + kTrialMs;
    return status;
}

DemoLedger::StartResult DemoLedger::start(std::int64_t nowMs, std::uint32_t serial)
{
    if (readAll().present)
        return StartResult::alreadyUsed;
    Record record;
    record.startedMs = nowMs;
    record.lastSeenMs = nowMs;
    record.serial = serial;
    writeAll(record);

    // At least one place must really have kept it; otherwise the trial could start afresh each time.
    const auto text = seal(record);
    for (const auto& store : stores_)
        if (store->isDurable() && store->read() == text)
            return StartResult::started;
    return StartResult::notRecorded;
}

} // namespace dojo::licensing
