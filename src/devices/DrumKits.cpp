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

juce::Array<juce::File>& extraArchiveFolders()
{
    static juce::Array<juce::File> folders;
    return folders;
}

/** A machine's archive in the binary data (null when there is none). */
const char* embeddedArchiveOf(const juce::String& machine, int& size)
{
    size = 0;
    for (int i = 0; i < DojoBinaryData::namedResourceListSize; ++i)
        if (juce::String(DojoBinaryData::originalFilenames[i]) == machine + kArchiveExtension)
            return DojoBinaryData::getNamedResource(DojoBinaryData::namedResourceList[i], size);
    return nullptr;
}

/** A machine's archive file beside the application (or in an added folder); none when there is none. */
juce::File archiveFileOf(const juce::String& machine)
{
    for (const auto& folder : getArchiveFolders())
        if (const auto file = folder.getChildFile(machine + kArchiveExtension); file.existsAsFile())
            return file;
    return {};
}

/** A machine's archive: compiled in, or a file beside the application. Null when there is neither. */
std::unique_ptr<juce::ZipFile> openArchive(const juce::String& machine)
{
    int size = 0;
    if (const auto* data = embeddedArchiveOf(machine, size); data != nullptr && size > 0)
        return std::make_unique<juce::ZipFile>(std::make_unique<juce::MemoryInputStream>(data, static_cast<std::size_t>(size), false));
    if (const auto file = archiveFileOf(machine); file != juce::File())
        return std::make_unique<juce::ZipFile>(file);
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
    const auto archive = openArchive(machine);
    if (archive == nullptr)
    {
        error = "There is no built-in drum machine called \"" + machine + "\" (its samples belong in " +
                getArchiveFolders().getFirst().getFullPathName() + ").";
        return false;
    }
    auto& zip = *archive;
    const auto folder = getLibraryFolder().getChildFile(machine);
    if (!folder.createDirectory())
    {
        error = "Could not create " + folder.getFullPathName();
        return false;
    }
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
            machines.addIfNotAlreadyThere(original.dropLastCharacters(static_cast<int>(std::strlen(kArchiveExtension))));
    }
    for (const auto& folder : getArchiveFolders())
        for (const auto& file : folder.findChildFiles(juce::File::findFiles, false, juce::String("*") + kArchiveExtension))
            machines.addIfNotAlreadyThere(file.getFileNameWithoutExtension());
    machines.sortNatural();
    return machines;
}

juce::StringArray getSampleNames(const juce::String& machine)
{
    juce::StringArray names;
    const auto archive = openArchive(machine);
    if (archive == nullptr)
        return names;
    for (int i = 0; i < archive->getNumEntries(); ++i)
        if (const auto* entry = archive->getEntry(i); entry != nullptr && isPlainName(entry->filename))
            names.add(entry->filename);
    names.sortNatural();
    return names;
}

juce::Array<juce::File> getArchiveFolders()
{
    juce::Array<juce::File> folders;
    folders.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getChildFile(kArchiveFolderName));
    folders.addArray(extraArchiveFolders());
    return folders;
}

void addArchiveFolder(const juce::File& folder)
{
    const std::lock_guard lock(mutex());
    extraArchiveFolders().addIfNotAlreadyThere(folder);
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
