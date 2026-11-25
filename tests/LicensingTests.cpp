#include "licensing/CaptureGuard.h"
#include "licensing/Crypto.h"
#include "licensing/DemoLedger.h"
#include "licensing/Features.h"
#include "licensing/LicenseKey.h"
#include "licensing/LicenseManager.h"
#include "licensing/Obfuscation.h"
#include "licensing/Integrity.h"
#include "licensing/PublicKey.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>

using namespace dojo::licensing;

namespace
{
template <std::size_t N>
std::array<std::uint8_t, N> fromHex(const char* hex)
{
    std::array<std::uint8_t, N> out{};
    juce::MemoryBlock block;
    block.loadFromHexString(hex);
    if (block.getSize() == N) // (also used before Catch runs: no REQUIRE here)
        std::memcpy(out.data(), block.getData(), N);
    return out;
}

// A key pair for the tests only (RFC 8032's first test vector); The Dojo trusts another one.
const auto kTestSeed = fromHex<32>("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
const auto kOtherSeed = fromHex<32>("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb");

/** The free-trial key the README publishes (signed by the production private key). */
constexpr const char* kPublishedTrialKey =
    "DOJO1-2G00009K-HEPSBMAA-G3CWGJCM-FWW0JGSD-7C534SB3-A7912702-BE908NF3-Q3TX7HSM-HKSGC2S9-W0K9XTXD-P6XS1TB1-B1JZ0A79-8A0EY8JG-CX40J444";

juce::String key(Tier tier, std::uint32_t serial, const crypto::Seed& seed = kTestSeed)
{
    return makeKey(Payload{tier, serial}, seed);
}

/** A manager over memory stores and a clock the test sets; the stores outlive managers. */
struct Rig
{
    std::int64_t now = 1'800'000'000'000; // (2027)
    MemoryMarkerStore* keyStore = nullptr;
    std::vector<MemoryMarkerStore*> ledgerStores;
    std::shared_ptr<std::vector<std::optional<juce::String>>> persisted = std::make_shared<std::vector<std::optional<juce::String>>>(4);
    std::shared_ptr<std::optional<juce::String>> persistedKey = std::make_shared<std::optional<juce::String>>();
    juce::String machine = "machine-A";

    /** A store that keeps its text in `slot` (so a new manager sees what an older one wrote). */
    struct SharedStore final : MarkerStore
    {
        explicit SharedStore(std::optional<juce::String>& slot) : slot_(slot) {}
        std::optional<juce::String> read() const override { return slot_; }
        void write(const juce::String& text) override { slot_ = text; }
        std::optional<juce::String>& slot_;
    };

    std::unique_ptr<LicenseManager> make()
    {
        LicenseManager::Config config;
        config.publicKey = crypto::publicKeyOf(kTestSeed);
        config.keyStore = std::make_unique<SharedStore>(*persistedKey);
        for (auto& slot : *persisted)
            config.ledgerStores.push_back(std::make_unique<SharedStore>(slot));
        config.machineId = machine;
        config.clock = [this] { return now; };
        return std::make_unique<LicenseManager>(std::move(config));
    }
};

constexpr std::int64_t kDay = 24LL * 60 * 60 * 1000;
} // namespace

TEST_CASE("Licensing: Ed25519 matches RFC 8032", "[licensing]")
{
    // TEST 1 (empty message), TEST 2 (one byte), TEST 3 (two bytes).
    struct Vector
    {
        const char* seed;
        const char* publicKey;
        const char* message;
        const char* signature;
    };
    const Vector vectors[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    };
    for (const auto& v : vectors)
    {
        const auto seed = fromHex<32>(v.seed);
        juce::MemoryBlock message;
        message.loadFromHexString(v.message);
        const std::span<const std::uint8_t> bytes{static_cast<const std::uint8_t*>(message.getData()), message.getSize()};
        CHECK(crypto::publicKeyOf(seed) == fromHex<32>(v.publicKey));
        const auto signature = crypto::sign(seed, bytes);
        CHECK(signature == fromHex<64>(v.signature));
        CHECK(crypto::verify(fromHex<32>(v.publicKey), bytes, signature));
        auto bad = signature;
        bad[10] ^= 1;
        CHECK_FALSE(crypto::verify(fromHex<32>(v.publicKey), bytes, bad));
    }
}

TEST_CASE("Licensing: Crockford Base32 and the key's check", "[licensing]")
{
    const std::uint8_t data[] = {0x00, 0x01, 0x00, 0x01, 0xff, 0x80};
    const auto text = toBase32(data, sizeof(data));
    CHECK(text.length() == 10);
    std::uint8_t back[sizeof(data)]{};
    REQUIRE(fromBase32(text, back, sizeof(back)));
    CHECK(std::memcmp(back, data, sizeof(data)) == 0);
    CHECK(text.containsOnly("0123456789ABCDEFGHJKMNPQRSTVWXYZ"));

    // CRC-16/CCITT-FALSE's check value.
    const char* check = "123456789";
    CHECK(crc16(reinterpret_cast<const std::uint8_t*>(check), 9) == 0x29B1);
}

TEST_CASE("Licensing: keys are signed, checked and read leniently", "[licensing]")
{
    const auto publicKey = crypto::publicKeyOf(kTestSeed);
    const auto pro = key(Tier::pro, 4242);
    CHECK(pro.startsWith("DOJO1-"));
    CHECK(pro.length() == 6 + 112 + 13);

    juce::String error;
    auto payload = verifyKey(pro, publicKey, error);
    REQUIRE(payload.has_value());
    CHECK(payload->tier == Tier::pro);
    CHECK(payload->serial == 4242u);

    // Lower case, no dashes, spaces, line breaks; O for 0 and l / I for 1.
    auto relaxed = pro.toLowerCase().removeCharacters("-");
    relaxed = relaxed.substring(0, 40) + " \r\n " + relaxed.substring(40);
    CHECK(verifyKey(relaxed, publicKey, error).has_value());
    CHECK(verifyKey(pro.replaceCharacter('0', 'O').replaceFirstOccurrenceOf("DOJ1", "DOJO1"), publicKey, error).has_value());
    CHECK(normaliseKey(relaxed) == pro);

    SECTION("a typo is caught by the check")
    {
        auto typo = pro;
        const int at = 20;
        const auto c = typo[at];
        typo = typo.substring(0, at) + juce::String::charToString(c == 'A' ? 'B' : 'A') + typo.substring(at + 1);
        CHECK_FALSE(verifyKey(typo, publicKey, error).has_value());
        CHECK(error.contains("typo"));
    }
    SECTION("wrong length, prefix or characters")
    {
        CHECK_FALSE(verifyKey(pro.dropLastCharacters(3), publicKey, error).has_value());
        CHECK(error.contains("112"));
        CHECK_FALSE(verifyKey(pro.replace("DOJO1", "DOJO2"), publicKey, error).has_value());
        CHECK_FALSE(verifyKey(pro.substring(0, 30) + "U" + pro.substring(31), publicKey, error).has_value());
        CHECK_FALSE(verifyKey("", publicKey, error).has_value());
        CHECK_FALSE(verifyKey("DOJO1-" + juce::String::repeatedString("0", 112), publicKey, error).has_value());
    }
    SECTION("another key pair's keys do not work")
    {
        CHECK_FALSE(verifyKey(key(Tier::pro, 1, kOtherSeed), publicKey, error).has_value());
        CHECK(error == "This license key is not valid.");
    }
    SECTION("changing the tier (with a correct check) breaks the signature")
    {
        auto parsed = parseKey(key(Tier::basic, 7), error);
        REQUIRE(parsed.has_value());
        auto forged = parsed->payload;
        forged.tier = Tier::pro;
        CHECK_FALSE(verifyKey(encodeKey(forged, parsed->signature), publicKey, error).has_value());
        forged = parsed->payload;
        forged.serial = 8;
        CHECK_FALSE(verifyKey(encodeKey(forged, parsed->signature), publicKey, error).has_value());
    }
    SECTION("a signature of the bare payload (no domain) is not a key")
    {
        const Payload p{Tier::pro, 9};
        const auto bytes = p.toBytes();
        CHECK_FALSE(verifyKey(encodeKey(p, crypto::sign(kTestSeed, bytes)), publicKey, error).has_value());
    }
    SECTION("a malleated signature (S + L) is refused")
    {
        auto parsed = parseKey(pro, error);
        REQUIRE(parsed.has_value());
        // L = 2^252 + 27742317777372353535851937790883648493, little-endian.
        const auto l = fromHex<32>("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010");
        auto sig = parsed->signature;
        int carry = 0;
        for (int i = 0; i < 32; ++i)
        {
            const int sum = sig[static_cast<std::size_t>(32 + i)] + l[static_cast<std::size_t>(i)] + carry;
            sig[static_cast<std::size_t>(32 + i)] = static_cast<std::uint8_t>(sum & 0xff);
            carry = sum >> 8;
        }
        CHECK_FALSE(verifyKey(encodeKey(parsed->payload, sig), publicKey, error).has_value());
    }
    SECTION("a payload of an unknown version or tier is refused")
    {
        std::array<std::uint8_t, 4> bytes{0x20 | (3 << 2), 0, 0, 1};
        CHECK_FALSE(Payload::fromBytes(bytes).has_value());
        bytes = {0x10, 0, 0, 1}; // tier 0
        CHECK_FALSE(Payload::fromBytes(bytes).has_value());
        bytes = {0x10 | (2 << 2) | 1, 0, 0, 1}; // reserved bits
        CHECK_FALSE(Payload::fromBytes(bytes).has_value());
    }
}

TEST_CASE("Licensing: the published free-trial key works with the built-in public key", "[licensing]")
{
    juce::String error;
    const auto payload = verifyKey(kPublishedTrialKey, getProductionPublicKey(), error);
    INFO(error);
    REQUIRE(payload.has_value());
    CHECK(payload->tier == Tier::demo);
    CHECK(payload->serial == 1u);
    CHECK_FALSE(isRevoked(*payload));
    // Revoked keys (the two test keys made while building this) are refused by serial.
    CHECK(isRevoked(Payload{Tier::basic, 100}));
    CHECK(isRevoked(Payload{Tier::pro, 101}));
    CHECK_FALSE(isRevoked(Payload{Tier::pro, 100}));
    // The test key pair is not the production one.
    CHECK_FALSE(verifyKey(key(Tier::pro, 1), getProductionPublicKey(), error).has_value());
}

TEST_CASE("Licensing: what each license allows", "[licensing]")
{
    const auto none = featuresOf(Tier::none);
    const auto demo = featuresOf(Tier::demo);
    const auto basic = featuresOf(Tier::basic);
    const auto pro = featuresOf(Tier::pro);
    CHECK(none == 0u);
    CHECK(has(demo, Feature::use));
    CHECK_FALSE(has(demo, Feature::exporting));
    CHECK(has(demo, Feature::surround));
    CHECK(has(demo, Feature::proDevices));
    CHECK(has(basic, Feature::exporting));
    CHECK_FALSE(has(basic, Feature::surround));
    CHECK_FALSE(has(basic, Feature::immersive));
    CHECK_FALSE(has(basic, Feature::proDevices));
    for (auto f : {Feature::use, Feature::exporting, Feature::surround, Feature::immersive, Feature::proDevices})
        CHECK(has(pro, f));

    CHECK(isProDevice("builtin:tape800"));
    CHECK(isProDevice("builtin:harrison4032"));
    CHECK(isProDevice("builtin:tg12345"));
    CHECK(isProDevice("builtin:gluecomp"));
    CHECK(isProDevice("builtin:rhodes73"));
    CHECK_FALSE(isProDevice("builtin:eq"));

    gate::reset();
    CHECK(gate::allows(Feature::exporting)); // (nothing published: everything)
    gate::publish(Tier::basic);
    CHECK(gate::allows(Feature::exporting));
    CHECK_FALSE(gate::allows(Feature::surround));
    gate::publish(Tier::none);
    CHECK_FALSE(gate::allows(Feature::use));
    gate::reset();
}

TEST_CASE("Licensing: the free trial's ledger", "[licensing]")
{
    std::vector<std::optional<juce::String>> slots(3);
    const auto makeLedger = [&](const juce::String& machine)
    {
        std::vector<std::unique_ptr<MarkerStore>> stores;
        for (auto& slot : slots)
            stores.push_back(std::make_unique<Rig::SharedStore>(slot));
        return DemoLedger(std::move(stores), machine);
    };
    const std::int64_t t0 = 1'800'000'000'000;

    auto ledger = makeLedger("A");
    CHECK(ledger.evaluate(t0).state == DemoLedger::State::unused);
    REQUIRE(ledger.start(t0, 1) == DemoLedger::StartResult::started);
    CHECK(ledger.start(t0 + 1000, 1) == DemoLedger::StartResult::alreadyUsed); // once only
    auto status = ledger.evaluate(t0 + 3 * kDay);
    CHECK(status.state == DemoLedger::State::active);
    CHECK(status.endsMs == t0 + 7 * kDay);
    for (const auto& slot : slots)
        CHECK(slot.has_value());

    SECTION("seven days later it has ended, for good")
    {
        CHECK(ledger.evaluate(t0 + 7 * kDay).state == DemoLedger::State::ended);
        CHECK(ledger.evaluate(t0 + 1 * kDay).state == DemoLedger::State::ended); // (and the clock going back does not revive it)
    }
    SECTION("turning the clock back ends it")
    {
        (void)ledger.evaluate(t0 + 5 * kDay);
        const auto back = ledger.evaluate(t0 + 2 * kDay);
        CHECK(back.state == DemoLedger::State::ended);
        CHECK(back.reason.contains("clock"));
    }
    SECTION("a small correction of the clock is fine")
    {
        (void)ledger.evaluate(t0 + 5 * kDay);
        CHECK(ledger.evaluate(t0 + 5 * kDay - 10 * 60 * 1000).state == DemoLedger::State::active);
    }
    SECTION("an edited record ends it")
    {
        juce::MemoryOutputStream decoded;
        REQUIRE(juce::Base64::convertFromBase64(decoded, *slots[1]));
        auto text = decoded.toString();
        text = text.replace(juce::String(t0), juce::String(t0 + 6 * kDay)); // a later start
        slots[1] = juce::Base64::toBase64(text);
        CHECK(ledger.evaluate(t0 + 3 * kDay).state == DemoLedger::State::ended);
    }
    SECTION("garbage in a store ends it")
    {
        slots[2] = juce::String("hello");
        CHECK(ledger.evaluate(t0 + 3 * kDay).state == DemoLedger::State::ended);
    }
    SECTION("deleting some copies does not help: they come back")
    {
        slots[0].reset();
        slots[1].reset();
        CHECK(ledger.evaluate(t0 + 3 * kDay).state == DemoLedger::State::active);
        CHECK(slots[0].has_value());
        CHECK(slots[1].has_value());
        CHECK(ledger.start(t0 + 3 * kDay, 1) == DemoLedger::StartResult::alreadyUsed);
    }
    SECTION("a record copied from another computer does not unseal")
    {
        auto other = makeLedger("B");
        CHECK(other.evaluate(t0 + 3 * kDay).state == DemoLedger::State::ended);
    }
}

TEST_CASE("Licensing: the trial cannot start where nothing can be kept, nor outlive the file system's clock", "[licensing]")
{
    const std::int64_t t0 = 1'800'000'000'000;

    // Read-only folders: every write is lost. The trial must not start (or it would start afresh each time).
    struct LostStore final : MarkerStore
    {
        std::optional<juce::String> read() const override { return std::nullopt; }
        void write(const juce::String&) override {}
    };
    {
        std::vector<std::unique_ptr<MarkerStore>> stores;
        stores.push_back(std::make_unique<LostStore>());
        stores.push_back(std::make_unique<LostStore>());
        DemoLedger ledger(std::move(stores), "A");
        CHECK(ledger.start(t0, 1) == DemoLedger::StartResult::notRecorded);
    }
    // A settings file that keeps it in memory only (it may fail to save) is not enough on its own.
    {
        juce::TemporaryFile tempSettings(".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "test";
        juce::PropertiesFile properties(tempSettings.getFile(), options);
        std::vector<std::unique_ptr<MarkerStore>> stores;
        stores.push_back(std::make_unique<PropertiesMarkerStore>(properties, "x"));
        stores.push_back(std::make_unique<LostStore>());
        DemoLedger ledger(std::move(stores), "A");
        CHECK(ledger.start(t0, 1) == DemoLedger::StartResult::notRecorded);
    }
    // A clock held still (a program faking the time to The Dojo alone): the files' own times move on.
    {
        juce::TemporaryFile temp;
        const auto file = temp.getFile();
        std::vector<std::unique_ptr<MarkerStore>> stores;
        stores.push_back(std::make_unique<FileMarkerStore>(file));
        DemoLedger ledger(std::move(stores), "A");
        const auto realNow = juce::Time::currentTimeMillis();
        REQUIRE(ledger.start(realNow, 1) == DemoLedger::StartResult::started);
        CHECK(ledger.evaluate(realNow).state == DemoLedger::State::active);
        // The file system says eight days have gone by; the clock The Dojo is shown says not.
        REQUIRE(file.setLastModificationTime(juce::Time(realNow + 8 * kDay)));
        CHECK(ledger.evaluate(realNow).state == DemoLedger::State::ended);
    }
}

TEST_CASE("Licensing: the license manager", "[licensing]")
{
    Rig rig;
    auto manager = rig.make();
    CHECK(manager->getTier() == Tier::none);
    CHECK(manager->getStatus().message.contains("Enter your license key"));
    CHECK_FALSE(manager->allows(Feature::use));

    SECTION("not a key")
    {
        const auto result = manager->activate("DOJO1-NOTAKEY");
        CHECK_FALSE(result.ok);
        CHECK(manager->getTier() == Tier::none);
        CHECK_FALSE(rig.persistedKey->has_value());
    }
    SECTION("Basic and Pro keys work forever")
    {
        auto result = manager->activate(key(Tier::basic, 120));
        CHECK(result.ok);
        CHECK(manager->getTier() == Tier::basic);
        CHECK_FALSE(manager->allows(Feature::surround));
        rig.now += 3650 * kDay;
        manager->refresh();
        CHECK(manager->getTier() == Tier::basic);

        result = manager->activate(key(Tier::pro, 121));
        CHECK(result.ok);
        CHECK(manager->getTier() == Tier::pro);

        // It is read back (and checked again) on the next start.
        auto again = rig.make();
        CHECK(again->getTier() == Tier::pro);
        CHECK(again->getStatus().serial == 121u);

        // A trial key would only take features away.
        CHECK_FALSE(again->activate(key(Tier::demo, 1)).ok);
        CHECK(again->getTier() == Tier::pro);
    }
    SECTION("the stored key is checked every time: editing it does not upgrade")
    {
        REQUIRE(manager->activate(key(Tier::basic, 130)).ok);
        juce::String parseError;
        auto parsed = parseKey(**rig.persistedKey, parseError);
        REQUIRE(parsed.has_value());
        auto forged = parsed->payload;
        forged.tier = Tier::pro;
        *rig.persistedKey = encodeKey(forged, parsed->signature);
        auto again = rig.make();
        CHECK(again->getTier() == Tier::none);
        CHECK(again->getStatus().message.contains("does not work"));
    }
    SECTION("the free trial: seven days, once")
    {
        auto result = manager->activate(key(Tier::demo, 1));
        CHECK(result.ok);
        CHECK(manager->getTier() == Tier::demo);
        CHECK(manager->getTrialDaysLeft() == 7);
        CHECK_FALSE(manager->allows(Feature::exporting));
        CHECK(manager->allows(Feature::proDevices));

        rig.now += 6 * kDay + 1000;
        manager->refresh();
        CHECK(manager->getTier() == Tier::demo);
        CHECK(manager->getTrialDaysLeft() == 1);

        rig.now += kDay;
        manager->refresh();
        CHECK(manager->getTier() == Tier::none);
        CHECK(manager->getStatus().message.contains("ended"));

        // The same key, or any trial key, never works again here.
        result = manager->activate(key(Tier::demo, 1));
        CHECK_FALSE(result.ok);
        CHECK(result.message.contains("already been used"));
        CHECK_FALSE(manager->activate(key(Tier::demo, 2)).ok);

        // Removing the key does not reset it; a paid key still works.
        manager->removeKey();
        CHECK_FALSE(manager->activate(key(Tier::demo, 1)).ok);
        CHECK(manager->activate(key(Tier::pro, 200)).ok);
        CHECK(manager->getTier() == Tier::pro);
    }
    SECTION("removing the key while the trial runs, then entering it again, goes on (it does not restart)")
    {
        REQUIRE(manager->activate(key(Tier::demo, 1)).ok);
        rig.now += 3 * kDay;
        manager->removeKey();
        CHECK(manager->getTier() == Tier::none);
        CHECK(manager->getStatus().message.contains("still running"));
        REQUIRE(manager->activate(key(Tier::demo, 1)).ok);
        CHECK(manager->getTier() == Tier::demo);
        CHECK(manager->getTrialDaysLeft() == 4);
    }
    SECTION("the trial cannot be restarted by deleting its record while keeping the key")
    {
        REQUIRE(manager->activate(key(Tier::demo, 1)).ok);
        for (auto& slot : *rig.persisted)
            slot.reset();
        auto again = rig.make();
        CHECK(again->getTier() == Tier::none);
        CHECK(again->getStatus().message.contains("removed"));
        CHECK_FALSE(again->activate(key(Tier::demo, 1)).ok);
    }
    SECTION("turning the clock back during the trial ends it")
    {
        REQUIRE(manager->activate(key(Tier::demo, 1)).ok);
        rig.now += 4 * kDay;
        manager->refresh();
        rig.now -= 3 * kDay;
        manager->refresh();
        CHECK(manager->getTier() == Tier::none);
        CHECK(manager->getStatus().message.contains("clock"));
    }
    SECTION("a reinstall (new settings, the ledger's other copies kept) does not restore the trial")
    {
        REQUIRE(manager->activate(key(Tier::demo, 1)).ok);
        rig.now += 5 * kDay;
        rig.persistedKey->reset();
        (*rig.persisted)[0].reset();
        auto again = rig.make();
        CHECK(again->getTier() == Tier::none);
        CHECK(again->getStatus().message.contains("still running"));
        // The key again: the same trial goes on, with the days it had left (not seven new ones).
        REQUIRE(again->activate(key(Tier::demo, 1)).ok);
        CHECK(again->getTrialDaysLeft() == 2);
        // Once it has ended, a reinstall does not bring it back.
        rig.now += 3 * kDay;
        again->refresh();
        CHECK(again->getTier() == Tier::none);
        rig.persistedKey->reset();
        (*rig.persisted)[0].reset();
        auto third = rig.make();
        CHECK(third->getStatus().message.contains("ended"));
        CHECK_FALSE(third->activate(key(Tier::demo, 1)).ok);
    }
    SECTION("the application's manager publishes its license")
    {
        gate::reset();
        LicenseManager::Config config;
        config.publicKey = crypto::publicKeyOf(kTestSeed);
        config.keyStore = std::make_unique<MemoryMarkerStore>();
        config.machineId = "x";
        config.clock = [&] { return rig.now; };
        config.publishToGate = true;
        LicenseManager published(std::move(config));
        CHECK(gate::isPublished());
        CHECK_FALSE(gate::allows(Feature::use));
        REQUIRE(published.activate(key(Tier::basic, 5)).ok);
        CHECK(gate::tier() == Tier::basic);
        CHECK(gate::allows(Feature::exporting));
        CHECK_FALSE(gate::allows(Feature::proDevices));
        gate::reset();
    }
}

TEST_CASE("Licensing: recording software and virtual devices are recognised", "[licensing]")
{
    using namespace capture;
    auto found = matchRecordingSoftware({"explorer.exe", "obs64.exe", "voicemeeter8x64.exe", "VoicemeeterPro.exe", "chrome.exe"});
    CHECK(found.contains("OBS Studio"));
    CHECK(found.contains("Voicemeeter Potato"));
    CHECK(found.contains("Voicemeeter Banana"));
    CHECK_FALSE(found.contains("chrome"));
    found = matchRecordingSoftware({"bash", "simplescreenrec", "obs", "pw-record", "Xorg"}); // (Linux: names cut to 15)
    CHECK(found.contains("SimpleScreenRecorder"));
    CHECK(found.contains("OBS Studio"));
    CHECK(found.contains("pw-record"));
    CHECK(matchRecordingSoftware({"systemd", "TheDojo", "jackd", "pipewire"}).isEmpty());

    CHECK(isVirtualAudioDevice("CABLE Input (VB-Audio Virtual Cable)"));
    CHECK(isVirtualAudioDevice("VoiceMeeter Input (VB-Audio VoiceMeeter VAIO)"));
    CHECK(isVirtualAudioDevice("Voicemeeter AUX Input"));
    CHECK(isVirtualAudioDevice("BlackHole 2ch"));
    CHECK(isVirtualAudioDevice("Stereo Mix (Realtek(R) Audio)"));
    CHECK(isVirtualAudioDevice("Monitor of Built-in Audio Analog Stereo"));
    CHECK(isVirtualAudioDevice("Loopback, Loopback PCM"));
    CHECK_FALSE(isVirtualAudioDevice("Speakers (Realtek(R) Audio)"));
    CHECK_FALSE(isVirtualAudioDevice("Focusrite USB ASIO"));
    CHECK_FALSE(isVirtualAudioDevice("system"));
    (void)findRecordingSoftware(); // (lists this machine's processes without failing)
}

TEST_CASE("Licensing: keyed BLAKE2b matches the stamp tool (hashlib)", "[licensing]")
{
    // hashlib.blake2b(msg, digest_size=32, key=bytes(range(32))) for this message.
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(i);
    const juce::String message = "The Dojo integrity check vector";
    std::array<std::uint8_t, 32> out{};
    crypto::keyedHash(out, key, {reinterpret_cast<const std::uint8_t*>(message.toRawUTF8()), message.getNumBytesAsUTF8()});
    juce::MemoryBlock expected;
    expected.loadFromHexString("4c29f0a7926167a5ad68791a6f290a408daed6b51f85fe2cf7f62bf875f02e46");
    CHECK(crypto::equal(out, {static_cast<const std::uint8_t*>(expected.getData()), expected.getSize()}));
}

TEST_CASE("Licensing: string obfuscation round-trips", "[licensing]")
{
    CHECK(DOJO_OBF("DOJO1") == "DOJO1");
    CHECK(DOJO_OBF("TheDojo/license/v1") == "TheDojo/license/v1");
    CHECK(DOJO_OBF("") == "");
    CHECK(DOJO_OBF("a b-c_1/2.3!") == "a b-c_1/2.3!");
    // The encoded bytes differ from the plaintext (so it is not stored in the clear).
    static constexpr obf::Obfuscated<6> encoded("DOJO1");
    CHECK(std::memcmp(encoded.bytes.data(), "DOJO1", 5) != 0);
}

TEST_CASE("Licensing: the integrity self-check on an in-memory image", "[licensing]")
{
    using namespace integrity;
    const auto marker = detail::marker();

    std::vector<std::uint8_t> image;
    for (int i = 0; i < 500; ++i)
        image.push_back(static_cast<std::uint8_t>(i * 37 + 11));
    const auto markerAt = image.size();
    image.insert(image.end(), marker.begin(), marker.end());
    image.insert(image.end(), 32, 0); // the zero stamp field
    for (int i = 0; i < 500; ++i)
        image.push_back(static_cast<std::uint8_t>(i * 13 + 7));

    CHECK(detail::evaluateImage(image) == Status::unstamped); // not stamped yet
    REQUIRE(detail::stampImage(image));
    CHECK(detail::evaluateImage(image) == Status::ok);

    SECTION("a byte changed anywhere is caught")
    {
        auto bad = image;
        bad[10] ^= 0x01;
        CHECK(detail::evaluateImage(bad) == Status::tampered);
    }
    SECTION("changing the stored stamp is caught")
    {
        auto bad = image;
        bad[markerAt + marker.size()] ^= 0x80;
        CHECK(detail::evaluateImage(bad) == Status::tampered);
    }
    SECTION("re-stamping after a change makes it valid again (as the build does)")
    {
        auto changed = image;
        changed[20] ^= 0x01;
        REQUIRE(detail::stampImage(changed));
        CHECK(detail::evaluateImage(changed) == Status::ok);
    }
    SECTION("no marker: not enforced")
    {
        std::vector<std::uint8_t> plain(200, 0xab);
        CHECK(detail::evaluateImage(plain) == Status::unstamped);
    }
}

TEST_CASE("Licensing: a modified binary locks everything (gate tamper)", "[licensing]")
{
    gate::reset();
    gate::publish(Tier::pro);
    CHECK(gate::allows(Feature::use));
    gate::setTamperLocked(true);
    CHECK(gate::isTamperLocked());
    CHECK_FALSE(gate::allows(Feature::use));
    CHECK_FALSE(gate::allows(Feature::exporting));
    CHECK(gate::tier() == Tier::none);
    gate::reset();
    CHECK_FALSE(gate::isTamperLocked());
    CHECK(gate::allows(Feature::use));
}

TEST_CASE("Licensing: the embedded public key passes its own cross-check", "[licensing]")
{
    const auto key = getProductionPublicKey();
    CHECK_FALSE(std::all_of(key.begin(), key.end(), [](std::uint8_t b) { return b == 0; }));
}
