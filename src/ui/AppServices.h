#pragma once

#include "engine/EngineController.h"
#include "engine/Preview.h"
#include "licensing/LicenseManager.h"
#include "plugins/PluginManager.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "ui/Preferences.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/** Everything the UI works with, owned by the application (see DojoApplication). */
struct AppServices
{
    session::Project& project;
    engine::EngineController& controller;
    juce::ApplicationCommandManager& commands;
    plugins::PluginManager& plugins;
    session::Recorder& recorder;
    session::Exporter& exporter;
    juce::ApplicationProperties& settings;
    engine::PreviewService& preview;
    Preferences& preferences;
    /** Opens the Preferences window on a tab (set by the main window). */
    std::function<void(Preferences::Tab)> showPreferences;
    /** View > Full Screen: toggles it, and says whether it is on (set by the main window). */
    std::function<void()> toggleFullScreen;
    std::function<bool()> isFullScreen;
    /** The external renderer's state in words (the Renderer tab; set by the application). */
    std::function<juce::String()> describeExternalRenderer;
    /** The license (set by the application; null in tests that build the UI on their own). */
    licensing::LicenseManager* license = nullptr;
    /** The license in a few words for the status bar ("PRO", "TRIAL: 6 DAYS LEFT"...; set by the application). */
    std::function<juce::String()> describeLicense{};
    /** Options > License...: the license window (set by the main window). */
    std::function<void()> showLicense{};
};

} // namespace dojo::ui
