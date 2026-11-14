#pragma once

namespace juce
{
class ComponentPeer;
}

namespace dojo::app::windowmanager
{

/**
    The desktop window manager's own window states (EWMH, on Linux / X11 and XWayland).

    A window with its own title bar can be maximised by the window manager (by its placement policy,
    a keyboard shortcut, a double-click it intercepts, or because the window resized itself to the
    work area). While the window manager holds it maximised it ignores the window's own resize
    requests, so the window must ask it to let go. Elsewhere (Windows, macOS) these do nothing and
    report false: there the window's own bounds are all there is.
*/

/** Whether the window manager has the window maximised (both directions). */
[[nodiscard]] bool isMaximised(juce::ComponentPeer* peer);
/** Whether the window manager shows the window full screen. */
[[nodiscard]] bool isFullScreen(juce::ComponentPeer* peer);

/** Asks the window manager to maximise (or stop maximising) the window. */
void requestMaximised(juce::ComponentPeer* peer, bool maximised);
/** Asks the window manager to show the window full screen (above panels), or not. */
void requestFullScreen(juce::ComponentPeer* peer, bool fullScreen);

} // namespace dojo::app::windowmanager
