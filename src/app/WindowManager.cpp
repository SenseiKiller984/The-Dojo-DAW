// The X11 declarations of JUCE's window system are only visible with this set (and only here: the X
// headers define macros such as None and Bool that the rest of the code base must not see).
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "app/WindowManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

namespace dojo::app::windowmanager
{
#if JUCE_LINUX || JUCE_BSD
namespace
{
::Window windowOf(juce::ComponentPeer* peer)
{
    return peer != nullptr ? static_cast<::Window>(reinterpret_cast<juce::pointer_sized_uint>(peer->getNativeHandle())) : 0;
}

/** Whether the window's _NET_WM_STATE holds every one of `names`. */
bool hasStates(juce::ComponentPeer* peer, std::initializer_list<const char*> names)
{
    const auto window = windowOf(peer);
    auto* system = juce::XWindowSystem::getInstanceWithoutCreating();
    if (window == 0 || system == nullptr || system->getDisplay() == nullptr)
        return false;
    auto* display = system->getDisplay();
    juce::XWindowSystemUtilities::ScopedXLock lock;
    const auto stateAtom = juce::XWindowSystemUtilities::Atoms::getIfExists(display, "_NET_WM_STATE");
    if (stateAtom == None)
        return false;
    juce::XWindowSystemUtilities::GetXProperty property(display, window, stateAtom, 0, 128, false, XA_ATOM);
    if (!property.success || property.actualFormat != 32 || property.actualType != XA_ATOM)
        return false;
    const auto* data = juce::unalignedPointerCast<const long*>(property.data);
    const auto* end = data + property.numItems;
    for (const auto* name : names)
    {
        const auto atom = juce::XWindowSystemUtilities::Atoms::getIfExists(display, name);
        if (atom == None || std::find(data, end, static_cast<long>(atom)) == end)
            return false;
    }
    return true;
}

/** _NET_WM_STATE add (1) or remove (0) of up to two states, sent to the window manager. */
void requestStates(juce::ComponentPeer* peer, bool add, const char* first, const char* second)
{
    const auto window = windowOf(peer);
    auto* system = juce::XWindowSystem::getInstanceWithoutCreating();
    auto* symbols = juce::X11Symbols::getInstanceWithoutCreating();
    if (window == 0 || system == nullptr || symbols == nullptr || system->getDisplay() == nullptr)
        return;
    auto* display = system->getDisplay();
    juce::XWindowSystemUtilities::ScopedXLock lock;
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.format = 32;
    event.xclient.message_type = juce::XWindowSystemUtilities::Atoms::getCreating(display, "_NET_WM_STATE");
    event.xclient.data.l[0] = add ? 1 : 0;
    event.xclient.data.l[1] = static_cast<long>(juce::XWindowSystemUtilities::Atoms::getCreating(display, first));
    event.xclient.data.l[2] = second != nullptr ? static_cast<long>(juce::XWindowSystemUtilities::Atoms::getCreating(display, second)) : 0;
    event.xclient.data.l[3] = 1; // (from an application)
    event.xclient.data.l[4] = 0;
    const auto root = symbols->xRootWindow(display, symbols->xDefaultScreen(display));
    symbols->xSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    symbols->xFlush(display);
}
} // namespace

bool isMaximised(juce::ComponentPeer* peer)
{
    return hasStates(peer, {"_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_MAXIMIZED_VERT"});
}

bool isFullScreen(juce::ComponentPeer* peer)
{
    return hasStates(peer, {"_NET_WM_STATE_FULLSCREEN"});
}

void requestMaximised(juce::ComponentPeer* peer, bool maximised)
{
    requestStates(peer, maximised, "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_MAXIMIZED_VERT");
}

void requestFullScreen(juce::ComponentPeer* peer, bool fullScreen)
{
    requestStates(peer, fullScreen, "_NET_WM_STATE_FULLSCREEN", nullptr);
}

#else

bool isMaximised(juce::ComponentPeer*)
{
    return false;
}

bool isFullScreen(juce::ComponentPeer*)
{
    return false;
}

void requestMaximised(juce::ComponentPeer*, bool) {}
void requestFullScreen(juce::ComponentPeer*, bool) {}

#endif
} // namespace dojo::app::windowmanager
