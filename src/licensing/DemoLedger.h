#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

/**
    The free trial's ledger: whether this computer's one free trial has been used, when it started,
    and the latest time The Dojo saw (so turning the clock back ends it).

    The record is kept in several places at once (MarkerStore: the settings, files in the user's
    data folders, the Windows registry), each copy sealed with a keyed BLAKE2b hash bound to this
    computer. Reading takes them all together: any copy means the trial was used, the earliest start
    and the latest time seen win, and a copy that was edited (its seal broken) ends the trial. Every
    evaluation writes the merged record back everywhere, so deleting some copies brings them back.

    Without a server this cannot be absolute: removing every copy (and the license key) on a
    computer, or another computer, starts afresh. It stops the trial being reused by reinstalling,
    by editing or deleting a file, by copying the record from elsewhere or by turning the clock back.
*/
namespace dojo::licensing
{

/** One place the ledger's record is kept. */
class MarkerStore
{
public:
    virtual ~MarkerStore() = default;
    /** The stored text; nothing when there is none. */
    [[nodiscard]] virtual std::optional<juce::String> read() const = 0;
    virtual void write(const juce::String& text) = 0;
    /** Whether read() reflects what was really kept (a file read back from disk; not an in-memory
        copy of a settings file that may have failed to save). */
    [[nodiscard]] virtual bool isDurable() const { return true; }
    /** When the store was last written, by the file system's clock (files only). */
    [[nodiscard]] virtual std::optional<std::int64_t> lastWrittenMs() const { return std::nullopt; }
};

/** A file (created with its folder). */
class FileMarkerStore final : public MarkerStore
{
public:
    explicit FileMarkerStore(juce::File file) : file_(std::move(file)) {}
    [[nodiscard]] std::optional<juce::String> read() const override;
    void write(const juce::String& text) override;
    [[nodiscard]] std::optional<std::int64_t> lastWrittenMs() const override;

private:
    juce::File file_;
};

/** A value in a properties file (the application's settings). */
class PropertiesMarkerStore final : public MarkerStore
{
public:
    PropertiesMarkerStore(juce::PropertiesFile& properties, juce::String key) : properties_(properties), key_(std::move(key)) {}
    [[nodiscard]] std::optional<juce::String> read() const override;
    void write(const juce::String& text) override;
    [[nodiscard]] bool isDurable() const override { return false; }

private:
    juce::PropertiesFile& properties_;
    juce::String key_;
};

/** A string value under HKEY_CURRENT_USER (Windows only; elsewhere it keeps nothing). */
class RegistryMarkerStore final : public MarkerStore
{
public:
    explicit RegistryMarkerStore(juce::String valuePath) : valuePath_(std::move(valuePath)) {}
    [[nodiscard]] std::optional<juce::String> read() const override;
    void write(const juce::String& text) override;

private:
    juce::String valuePath_;
};

/** Kept in memory (tests). */
class MemoryMarkerStore final : public MarkerStore
{
public:
    [[nodiscard]] std::optional<juce::String> read() const override { return text_; }
    void write(const juce::String& text) override { text_ = text; }
    std::optional<juce::String> text_;
};

class DemoLedger
{
public:
    static constexpr std::int64_t kTrialMs = 7LL * 24 * 60 * 60 * 1000;
    /** How far the clock may go back (time zone and daylight changes do not move UTC; network time
        corrections are seconds) before the trial counts as tampered with. */
    static constexpr std::int64_t kRollbackToleranceMs = 60LL * 60 * 1000;

    enum class State
    {
        unused,   ///< this computer's trial has not been started
        active,   ///< started, still running
        ended,    ///< seven days are up, the record was tampered with or the clock went back
    };

    struct Status
    {
        State state = State::unused;
        std::int64_t startedMs = 0;
        std::int64_t endsMs = 0;
        juce::String reason; ///< why it ended
    };

    /** `machineId` binds the seals to this computer. */
    DemoLedger(std::vector<std::unique_ptr<MarkerStore>> stores, juce::String machineId);

    /** The trial's state at `nowMs` (UTC milliseconds); records the time seen everywhere. The time
        used is never earlier than when a store's file was last written by the file system (so a
        program faking the clock to The Dojo alone does not stop the trial's days). */
    Status evaluate(std::int64_t nowMs);

    enum class StartResult
    {
        started,
        alreadyUsed,  ///< nothing changes
        notRecorded,  ///< nothing could be kept (read-only folders): the trial is not started
    };
    /** Starts the trial at `nowMs`. */
    StartResult start(std::int64_t nowMs, std::uint32_t serial);

    /** The sealed text of a record (exposed for tests). */
    struct Record
    {
        std::int64_t startedMs = 0;
        std::int64_t lastSeenMs = 0;
        std::uint32_t serial = 0;
        bool ended = false;
    };
    [[nodiscard]] juce::String seal(const Record& record) const;
    /** A record from sealed text; nothing if the seal does not match (edited, or another computer's). */
    [[nodiscard]] std::optional<Record> unseal(const juce::String& text) const;

private:
    struct Merged
    {
        bool present = false;
        bool tampered = false;
        Record record;
    };
    [[nodiscard]] Merged readAll() const;
    void writeAll(const Record& record);

    std::vector<std::unique_ptr<MarkerStore>> stores_;
    std::array<std::uint8_t, 32> sealKey_{};
};

} // namespace dojo::licensing
