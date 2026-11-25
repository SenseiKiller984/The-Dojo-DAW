#include "licensing/CaptureGuard.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <tlhelp32.h>
#endif

namespace dojo::licensing::capture
{
namespace
{
struct Program
{
    const char* process; ///< lower case, without ".exe"
    const char* name;
};

// Programs that record (or reroute) what the sound card plays.
constexpr Program kRecorders[] = {
    {"obs", "OBS Studio"},
    {"obs32", "OBS Studio"},
    {"obs64", "OBS Studio"},
    {"obs-studio", "OBS Studio"},
    {"streamlabs obs", "Streamlabs"},
    {"streamlabs desktop", "Streamlabs"},
    {"slobs", "Streamlabs"},
    {"xsplit.core", "XSplit"},
    {"xsplitbroadcaster", "XSplit"},
    {"bandicam", "Bandicam"},
    {"bdcam", "Bandicam"},
    {"camtasia", "Camtasia"},
    {"camtasiastudio", "Camtasia"},
    {"camrecorder", "Camtasia"},
    {"sharex", "ShareX"},
    {"fraps", "Fraps"},
    {"voicemeeter", "Voicemeeter"},
    {"voicemeeterpro", "Voicemeeter Banana"},
    {"voicemeeterpro_x64", "Voicemeeter Banana"},
    {"voicemeeter_x64", "Voicemeeter"},
    {"voicemeeter8", "Voicemeeter Potato"},
    {"voicemeeter8x64", "Voicemeeter Potato"},
    {"audacity", "Audacity"},
    {"audiorelay", "AudioRelay"},
    {"simplescreenrecorder", "SimpleScreenRecorder"},
    {"kazam", "Kazam"},
    {"vokoscreen", "vokoscreen"},
    {"vokoscreenng", "vokoscreenNG"},
    {"recordmydesktop", "recordMyDesktop"},
    {"gpu-screen-recorder", "GPU Screen Recorder"},
    {"kooha", "Kooha"},
    {"pw-record", "pw-record"},
    {"parecord", "parecord"},
    {"parec", "parec"},
};

// Virtual and loopback audio devices (lower-case parts of their names).
constexpr const char* kVirtualDevices[] = {
    "voicemeeter", "vb-audio", "vb-cable", "cable input", "cable output", "cable in 16ch", "hi-fi cable", "blackhole",
    "soundflower", "loopback", "obs virtual", "obs-", "virtual audio", "virtual cable", "virtual speaker",
    "stereo mix", "what u hear", "wave out mix", "monitor of", "null output", "audiorelay", "streaming speakers",
};

juce::String normalise(juce::String name)
{
    name = name.trim().toLowerCase();
    if (name.endsWith(".exe"))
        name = name.dropLastCharacters(4);
    return name;
}

juce::StringArray listProcesses()
{
    juce::StringArray names;
#if JUCE_WINDOWS
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return names;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        names.add(juce::String(entry.szExeFile));
    CloseHandle(snapshot);
#elif JUCE_LINUX || JUCE_BSD
    for (const auto& entry : juce::RangedDirectoryIterator(juce::File("/proc"), false, "*", juce::File::findDirectories))
    {
        const auto& dir = entry.getFile();
        if (!dir.getFileName().containsOnly("0123456789"))
            continue;
        const auto comm = dir.getChildFile("comm").loadFileAsString().trim();
        if (comm.isNotEmpty())
            names.add(comm);
    }
#endif
    return names;
}
} // namespace

juce::StringArray matchRecordingSoftware(const juce::StringArray& processNames)
{
    juce::StringArray found;
    for (const auto& raw : processNames)
    {
        const auto name = normalise(raw);
        if (name.isEmpty())
            continue;
        for (const auto& program : kRecorders)
        {
            const juce::String process(program.process);
            // Linux shortens process names to 15 characters.
            if (name == process || (name.length() == 15 && process.length() > 15 && process.startsWith(name)))
            {
                found.addIfNotAlreadyThere(program.name);
                break;
            }
        }
        if (name.startsWith("voicemeeter"))
            found.addIfNotAlreadyThere("Voicemeeter");
    }
    return found;
}

juce::StringArray findRecordingSoftware()
{
    return matchRecordingSoftware(listProcesses());
}

bool setWindowExcludedFromCapture([[maybe_unused]] void* nativeWindowHandle, [[maybe_unused]] bool excluded)
{
#if JUCE_WINDOWS
    if (nativeWindowHandle == nullptr)
        return false;
    const auto window = static_cast<HWND>(nativeWindowHandle);
    constexpr DWORD kExcludeFromCapture = 0x00000011; // WDA_EXCLUDEFROMCAPTURE (Windows 10 2004)
    constexpr DWORD kMonitor = 0x00000001;            // WDA_MONITOR
    if (!excluded)
        return SetWindowDisplayAffinity(window, 0) != FALSE;
    return SetWindowDisplayAffinity(window, kExcludeFromCapture) != FALSE || SetWindowDisplayAffinity(window, kMonitor) != FALSE;
#else
    return false;
#endif
}

bool isVirtualAudioDevice(const juce::String& deviceName)
{
    const auto name = deviceName.toLowerCase();
    for (const auto* part : kVirtualDevices)
        if (name.contains(part))
            return true;
    return false;
}

} // namespace dojo::licensing::capture
