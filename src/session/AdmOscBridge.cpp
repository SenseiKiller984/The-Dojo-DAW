#include "session/AdmOscBridge.h"

#include "engine/AudioEngine.h"
#include "engine/io/AdmMetadata.h"
#include "session/Project.h"

#include <cmath>
#include <cstring>

namespace dojo::session
{
namespace osc
{
namespace
{
void writePadded(juce::MemoryOutputStream& out, const juce::String& text)
{
    const auto utf8 = text.toUTF8();
    const auto length = std::strlen(utf8.getAddress());
    out.write(utf8.getAddress(), length);
    for (auto n = length; n < ((length + 4) & ~static_cast<std::size_t>(3)); ++n)
        out.writeByte(0); // (at least one zero, up to a multiple of four)
}

void writeBigEndian(juce::MemoryOutputStream& out, std::uint32_t value)
{
    out.writeByte(static_cast<char>((value >> 24) & 0xff));
    out.writeByte(static_cast<char>((value >> 16) & 0xff));
    out.writeByte(static_cast<char>((value >> 8) & 0xff));
    out.writeByte(static_cast<char>(value & 0xff));
}

/** Reads a padded string at `at`, moving past it; false if it runs off the end. */
bool readPadded(const std::uint8_t* data, std::size_t size, std::size_t& at, juce::String& text)
{
    std::size_t end = at;
    while (end < size && data[end] != 0)
        ++end;
    if (end >= size)
        return false;
    text = juce::String::fromUTF8(reinterpret_cast<const char*>(data + at), static_cast<int>(end - at));
    at = (end + 4) & ~static_cast<std::size_t>(3);
    return at <= size;
}

bool readBigEndian(const std::uint8_t* data, std::size_t size, std::size_t& at, std::uint32_t& value)
{
    if (at + 4 > size)
        return false;
    value = (static_cast<std::uint32_t>(data[at]) << 24) | (static_cast<std::uint32_t>(data[at + 1]) << 16)
            | (static_cast<std::uint32_t>(data[at + 2]) << 8) | static_cast<std::uint32_t>(data[at + 3]);
    at += 4;
    return true;
}

void decodeInto(const std::uint8_t* data, std::size_t size, std::vector<Message>& messages, int depth)
{
    if (size < 8 || depth > 8)
        return;
    if (std::memcmp(data, "#bundle", 8) == 0)
    {
        std::size_t at = 16; // (the name and the time tag)
        while (at + 4 <= size)
        {
            std::uint32_t length = 0;
            readBigEndian(data, size, at, length);
            if (length > size - at)
                return;
            decodeInto(data + at, length, messages, depth + 1);
            at += length;
        }
        return;
    }
    Message message;
    std::size_t at = 0;
    juce::String tags;
    if (!readPadded(data, size, at, message.address) || !message.address.startsWithChar('/') || !readPadded(data, size, at, tags)
        || !tags.startsWithChar(','))
        return;
    for (int t = 1; t < tags.length(); ++t)
    {
        Argument argument;
        argument.type = static_cast<char>(tags[t]);
        std::uint32_t raw = 0;
        switch (argument.type)
        {
        case 'i':
            if (!readBigEndian(data, size, at, raw))
                return;
            argument.i = static_cast<std::int32_t>(raw);
            break;
        case 'f':
            if (!readBigEndian(data, size, at, raw))
                return;
            std::memcpy(&argument.f, &raw, sizeof(float));
            break;
        case 's':
            if (!readPadded(data, size, at, argument.s))
                return;
            break;
        default:
            return; // (a type this reader does not know)
        }
        message.arguments.push_back(argument);
    }
    messages.push_back(std::move(message));
}
} // namespace

Argument Argument::of(float value)
{
    Argument a;
    a.type = 'f';
    a.f = value;
    return a;
}

Argument Argument::of(int value)
{
    Argument a;
    a.type = 'i';
    a.i = value;
    return a;
}

Argument Argument::of(const juce::String& value)
{
    Argument a;
    a.type = 's';
    a.s = value;
    return a;
}

juce::MemoryBlock encode(const Message& message)
{
    juce::MemoryOutputStream out;
    writePadded(out, message.address);
    juce::String tags(",");
    for (const auto& a : message.arguments)
        tags += juce::String::charToString(static_cast<juce::juce_wchar>(a.type));
    writePadded(out, tags);
    for (const auto& a : message.arguments)
    {
        if (a.type == 'i')
            writeBigEndian(out, static_cast<std::uint32_t>(a.i));
        else if (a.type == 'f')
        {
            std::uint32_t raw = 0;
            std::memcpy(&raw, &a.f, sizeof(float));
            writeBigEndian(out, raw);
        }
        else
            writePadded(out, a.s);
    }
    return out.getMemoryBlock();
}

juce::MemoryBlock encodeBundle(const std::vector<Message>& messages)
{
    juce::MemoryOutputStream out;
    out.write("#bundle", 8); // (with its zero)
    writeBigEndian(out, 0);
    writeBigEndian(out, 1); // time tag 1: at once
    for (const auto& m : messages)
    {
        const auto bytes = encode(m);
        writeBigEndian(out, static_cast<std::uint32_t>(bytes.getSize()));
        out.write(bytes.getData(), bytes.getSize());
    }
    return out.getMemoryBlock();
}

std::vector<Message> decode(const void* data, std::size_t size)
{
    std::vector<Message> messages;
    decodeInto(static_cast<const std::uint8_t*>(data), size, messages, 0);
    return messages;
}
} // namespace osc

AdmOscBridge::AdmOscBridge(Project& project, engine::EngineController& controller)
    : project_(project)
    , controller_(controller)
{
}

AdmOscBridge::~AdmOscBridge()
{
    stopTimer();
}

void AdmOscBridge::setSettings(const Settings& settings)
{
    settings_ = settings;
    settings_.rate = std::clamp(settings_.rate, 10, 100);
    settings_.port = std::clamp(settings_.port, 1, 65535);
    stopTimer();
    socket_.reset();
    sent_.clear();
    error_.clear();
    if (!settings_.enabled)
        return;
    socket_ = std::make_unique<juce::DatagramSocket>(false); // (it takes a port of its own on its first send)
    ticksSinceEverything_ = settings_.rate; // (everything, first)
    startTimerHz(settings_.rate);
}

std::vector<AdmOscBridge::Object> AdmOscBridge::currentObjects() const
{
    std::vector<Object> objects;
    const auto& session = project_.getSession();
    const auto& engine = controller_.getEngine();
    int number = 1;
    for (const auto& t : session.getAllTracks())
    {
        if (!track::isObject(t) || !track::isPan3d(t) || !Session::isEffectivelyActive(t) || isMultichannel(track::getFormat(t)))
            continue;
        // Where it is: the engine's moving values (automation, a modulator), else its settings.
        std::array<double, 4> position{track::getAzimuth(t), track::getElevation(t), track::getDistance(t), track::getSize(t)};
        const int slot = project_.getCompiler().getSlot(track::getId(t));
        for (int k = 0; k < 4 && slot >= 0; ++k)
            if (const float moving = engine.getMovingSpatial(slot, k); !std::isnan(moving))
                position[static_cast<std::size_t>(k)] = static_cast<double>(moving);
        const bool stereo = track::getFormat(t) != TrackFormat::mono;
        for (int side = 0; side < (stereo ? 2 : 1); ++side)
        {
            Object o;
            o.number = number++;
            o.trackId = track::getId(t);
            o.name = track::getName(t) + (stereo ? (side == 0 ? " L" : " R") : "");
            double azimuth = position[0] + (stereo ? (side == 0 ? 30.0 : -30.0) : 0.0);
            azimuth = std::fmod(azimuth + 540.0, 360.0) - 180.0;
            o.azimuth = azimuth;
            o.elevation = position[1];
            o.distance = std::clamp(position[2], 0.0, 1.0);
            o.size = std::clamp(position[3], 0.0, 1.0);
            o.audible = !track::isMuted(t);
            objects.push_back(o);
        }
    }
    return objects;
}

std::vector<osc::Message> AdmOscBridge::messagesNow(bool everything)
{
    std::vector<osc::Message> messages;
    for (const auto& o : currentObjects())
    {
        const std::array<double, 5> now{o.azimuth, o.elevation, o.distance, o.size, o.audible ? 1.0 : 0.0};
        const auto it = sent_.find(o.number);
        bool changed = everything || it == sent_.end();
        for (std::size_t k = 0; k < now.size() && !changed; ++k)
            changed = std::abs(now[k] - it->second[k]) > 1.0e-4;
        if (!changed)
            continue;
        sent_[o.number] = now;

        const auto base = "/adm/obj/" + juce::String(o.number) + "/";
        const auto f = [](double v) { return osc::Argument::of(static_cast<float>(v)); };
        if (settings_.polar || !settings_.cartesian)
            messages.push_back({base + "aed", {f(o.azimuth), f(o.elevation), f(o.distance)}});
        if (settings_.cartesian)
        {
            const auto c = engine::adm::polarToCartesian(o.azimuth, o.elevation, o.distance);
            messages.push_back({base + "xyz", {f(c.x), f(c.y), f(c.z)}});
        }
        messages.push_back({base + "w", {f(o.size)}});
        messages.push_back({base + "gain", {f(now[4])}});
        if (everything)
            messages.push_back({base + "name", {osc::Argument::of(o.name)}});
    }
    return messages;
}

void AdmOscBridge::sendNow(bool everything)
{
    if (socket_ == nullptr)
        return;
    const auto messages = messagesNow(everything);
    std::vector<osc::Message> bundle;
    std::size_t bytes = 16;
    const auto send = [&]
    {
        if (bundle.empty())
            return;
        const auto packet = osc::encodeBundle(bundle);
        if (socket_->write(settings_.host, settings_.port, packet.getData(), static_cast<int>(packet.getSize())) < 0)
            error_ = "Could not send to " + settings_.host + ":" + juce::String(settings_.port);
        else
            error_.clear();
        bundle.clear();
        bytes = 16;
    };
    for (const auto& m : messages)
    {
        const auto size = osc::encode(m).getSize() + 4;
        if (bytes + size > 1200)
            send();
        bundle.push_back(m);
        bytes += size;
    }
    send();
}

void AdmOscBridge::timerCallback()
{
    const bool everything = ++ticksSinceEverything_ >= settings_.rate; // (once a second)
    if (everything)
        ticksSinceEverything_ = 0;
    sendNow(everything);
}

} // namespace dojo::session
