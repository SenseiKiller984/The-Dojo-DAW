#include "devices/DrumKits.h"

#include <DojoBinaryData.h>

#include <cstring>
#include <mutex>

namespace dojo::devices::kits
{
namespace
{
constexpr const char* kArchiveExtension = ".zip";

std::mutex& mutex()
{
    static std::mutex m;
    return m;
}

juce::String& libraryOverride()
{
    static juce::String path;
    return path;
}

/** A machine's archive in the binary data (null when there is none). */
const char* archiveOf(const juce::String& machine, int& size)
{
    size = 0;
    for (int i = 0; i < DojoBinaryData::namedResourceListSize; ++i)
        if (juce::String(DojoBinaryData::originalFilenames[i]) == machine + kArchiveExtension)
            return DojoBinaryData::getNamedResource(DojoBinaryData::namedResourceList[i], size);
    return nullptr;
}

/** A sample name from an archive or a reference, safe as a file name (no folders, no "..). */
bool isPlainName(const juce::String& name)
{
    return name.isNotEmpty() && !name.containsAnyOf("/\\:") && name != "." && name != ".." && !name.startsWith(".");
}

/** Unpacks the archive's entries that are missing or of the wrong size (the lock held). */
bool unpackLocked(const juce::String& machine, juce::String& error)
{
    int size = 0;
    const auto* data = archiveOf(machine, size);
    if (data == nullptr || size <= 0)
    {
        error = "There is no built-in drum machine called \"" + machine + "\".";
        return false;
    }
    const auto folder = getLibraryFolder().getChildFile(machine);
    if (!folder.createDirectory())
    {
        error = "Could not create " + folder.getFullPathName();
        return false;
    }
    juce::MemoryInputStream archiveStream(data, static_cast<std::size_t>(size), false);
    juce::ZipFile zip(archiveStream);
    for (int i = 0; i < zip.getNumEntries(); ++i)
    {
        const auto* entry = zip.getEntry(i);
        if (entry == nullptr || entry->isSymbolicLink || !isPlainName(entry->filename))
            continue;
        const auto target = folder.getChildFile(entry->filename);
        if (target.existsAsFile() && target.getSize() == entry->uncompressedSize)
            continue;
        const std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
        if (in == nullptr)
        {
            error = "Could not read " + entry->filename + " from the built-in " + machine + " samples.";
            return false;
        }
        // Written beside it, then moved into place: a sample is either all there or not at all.
        juce::TemporaryFile temporary(target);
        {
            juce::FileOutputStream out(temporary.getFile());
            if (!out.openedOk() || out.writeFromInputStream(*in, -1) != entry->uncompressedSize)
            {
                error = "Could not write " + target.getFullPathName();
                return false;
            }
        }
        if (!temporary.overwriteTargetFileWithTemporary())
        {
            error = "Could not write " + target.getFullPathName();
            return false;
        }
    }
    return true;
}
} // namespace

juce::StringArray getMachines()
{
    juce::StringArray machines;
    for (int i = 0; i < DojoBinaryData::namedResourceListSize; ++i)
    {
        const juce::String original = DojoBinaryData::originalFilenames[i];
        if (original.endsWithIgnoreCase(kArchiveExtension))
            machines.add(original.dropLastCharacters(static_cast<int>(std::strlen(kArchiveExtension))));
    }
    machines.sortNatural();
    return machines;
}

juce::StringArray getSampleNames(const juce::String& machine)
{
    juce::StringArray names;
    int size = 0;
    const auto* data = archiveOf(machine, size);
    if (data == nullptr || size <= 0)
        return names;
    juce::MemoryInputStream archiveStream(data, static_cast<std::size_t>(size), false);
    juce::ZipFile zip(archiveStream);
    for (int i = 0; i < zip.getNumEntries(); ++i)
        if (const auto* entry = zip.getEntry(i); entry != nullptr && isPlainName(entry->filename))
            names.add(entry->filename);
    names.sortNatural();
    return names;
}

juce::File getLibraryFolder()
{
    {
        const auto& path = libraryOverride();
        if (path.isNotEmpty())
            return juce::File(path);
    }
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("The Dojo")
        .getChildFile("Library")
        .getChildFile("Drum Machines");
}

void setLibraryFolder(const juce::File& folder)
{
    const std::lock_guard lock(mutex());
    libraryOverride() = folder == juce::File() ? juce::String() : folder.getFullPathName();
}

bool isReference(const juce::String& text)
{
    return text.startsWith(kReferencePrefix);
}

juce::String referenceFor(const juce::File& file)
{
    const auto library = getLibraryFolder();
    if (file.isAChildOf(library))
    {
        const auto relative = file.getRelativePathFrom(library).replaceCharacter('\\', '/');
        const auto machine = relative.upToFirstOccurrenceOf("/", false, false);
        const auto name = relative.fromFirstOccurrenceOf("/", false, false);
        if (machine.isNotEmpty() && isPlainName(name) && getMachines().contains(machine))
            return kReferencePrefix + machine + "/" + name;
    }
    return file.getFullPathName();
}

juce::File fileFor(const juce::String& reference)
{
    if (!isReference(reference))
        return reference.isEmpty() ? juce::File() : juce::File(reference);
    const auto rest = reference.fromFirstOccurrenceOf(kReferencePrefix, false, false);
    const auto machine = rest.upToFirstOccurrenceOf("/", false, false);
    const auto name = rest.fromFirstOccurrenceOf("/", false, false);
    if (machine.isEmpty() || !isPlainName(name))
        return {};
    const std::lock_guard lock(mutex());
    const auto file = getLibraryFolder().getChildFile(machine).getChildFile(name);
    if (!file.existsAsFile())
    {
        juce::String error;
        if (!unpackLocked(machine, error))
        {
            juce::Logger::writeToLog("Drum kits: " + error);
            return {};
        }
    }
    return file.existsAsFile() ? file : juce::File();
}

bool unpack(const juce::String& machine, juce::String& error)
{
    const std::lock_guard lock(mutex());
    return unpackLocked(machine, error);
}

} // namespace dojo::devices::kits
