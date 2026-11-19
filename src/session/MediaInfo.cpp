#include "session/MediaInfo.h"

#include <cstring>
#include <functional>
#include <regex>

namespace dojo::session
{
namespace
{
/** The raw iXML chunk of a RIFF/WAVE file (empty if it has none). Reads chunk headers only. */
juce::String readIxmlChunk(const juce::File& file)
{
    juce::FileInputStream in(file);
    if (!in.openedOk())
        return {};
    char riff[4]{};
    if (in.read(riff, 4) != 4 || (std::memcmp(riff, "RIFF", 4) != 0 && std::memcmp(riff, "RF64", 4) != 0))
        return {};
    in.readInt(); // RIFF size
    char wave[4]{};
    if (in.read(wave, 4) != 4 || std::memcmp(wave, "WAVE", 4) != 0)
        return {};

    while (!in.isExhausted())
    {
        char id[4]{};
        if (in.read(id, 4) != 4)
            break;
        const auto size = static_cast<juce::int64>(static_cast<std::uint32_t>(in.readInt()));
        if (std::memcmp(id, "iXML", 4) == 0 && size > 0 && size < (1 << 20))
        {
            juce::MemoryBlock block;
            in.readIntoMemoryBlock(block, static_cast<juce::pointer_sized_int>(size));
            return block.toString();
        }
        if (!in.setPosition(in.getPosition() + size + (size & 1)))
            break;
    }
    return {};
}
} // namespace

void parseIxmlSceneTake(const juce::String& ixml, juce::String& scene, juce::String& take)
{
    scene.clear();
    take.clear();
    if (ixml.isEmpty())
        return;
    if (const auto xml = juce::XmlDocument::parse(ixml))
    {
        const std::function<void(const juce::XmlElement&)> visit = [&](const juce::XmlElement& e)
        {
            if (e.hasTagName("SCENE") && scene.isEmpty())
                scene = e.getAllSubText().trim();
            else if (e.hasTagName("TAKE") && take.isEmpty())
                take = e.getAllSubText().trim();
            for (const auto* child : e.getChildIterator())
                visit(*child);
        };
        visit(*xml);
    }
}

void parseSceneTakeFromName(const juce::String& name, juce::String& scene, juce::String& take)
{
    scene.clear();
    take.clear();
    static const std::regex pattern(R"((?:^|[^A-Za-z])(?:scene|sc|s)[ _\-.]?([0-9]+[A-Za-z]?)[ _\-.]*(?:take|tk|t)[ _\-.]?([0-9]+))",
                                    std::regex::icase);
    std::smatch match;
    const auto text = name.toStdString();
    if (std::regex_search(text, match, pattern))
    {
        scene = juce::String(match[1].str());
        take = juce::String(match[2].str());
    }
}

juce::String getDiskName(const juce::File& file)
{
#if JUCE_WINDOWS
    const auto path = file.getFullPathName();
    const auto drive = path.length() >= 2 && path[1] == ':' ? path.substring(0, 2).toUpperCase() : path.upToFirstOccurrenceOf("\\", false, false);
    const auto label = file.getVolumeLabel();
    return label.isNotEmpty() ? label + " (" + drive + ")" : drive;
#else
    // The longest mount point that holds the file.
    juce::String best = "/";
    const auto path = file.getFullPathName();
    const juce::File mounts("/proc/self/mounts");
    if (mounts.existsAsFile())
        for (const auto& line : juce::StringArray::fromLines(mounts.loadFileAsString()))
        {
            const auto point = juce::StringArray::fromTokens(line, " ", "")[1].replace("\\040", " ");
            if (point.isNotEmpty() && point.length() > best.length() && (path == point || path.startsWith(point + "/")))
                best = point;
        }
    const auto label = file.getVolumeLabel();
    return label.isNotEmpty() ? label : best;
#endif
}

MediaFileInfo readMediaFileInfo(juce::AudioFormatManager& formats, const juce::File& file)
{
    MediaFileInfo info;
    info.exists = file.existsAsFile();
    if (!info.exists)
        return info;
    info.created = file.getCreationTime();
    info.modified = file.getLastModificationTime();
    info.diskName = getDiskName(file);

    if (std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file)); reader != nullptr)
    {
        info.readable = true;
        info.sampleRate = reader->sampleRate;
        info.numChannels = static_cast<int>(reader->numChannels);
        info.lengthSeconds = reader->sampleRate > 0.0 ? static_cast<double>(reader->lengthInSamples) / reader->sampleRate : 0.0;
        const auto reference = reader->metadataValues.getValue(juce::WavAudioFormat::bwavTimeReference, {});
        if (reference.isNotEmpty() && reader->sampleRate > 0.0)
            info.timeReference = static_cast<double>(reference.getLargeIntValue()) / reader->sampleRate;
    }

    parseIxmlSceneTake(readIxmlChunk(file), info.scene, info.take);
    if (info.scene.isEmpty() && info.take.isEmpty())
        parseSceneTakeFromName(file.getFileNameWithoutExtension(), info.scene, info.take);
    return info;
}

} // namespace dojo::session
