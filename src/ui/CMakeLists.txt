add_library(dojo_ui STATIC
    MainComponent.cpp
    MainComponent.h
    PlaceholderSession.cpp
    PlaceholderSession.h
    components/ArrangerView.cpp
    components/ArrangerView.h
    components/BrowserPanel.cpp
    components/BrowserPanel.h
    components/IconButton.cpp
    components/IconButton.h
    components/LevelMeter.cpp
    components/LevelMeter.h
    components/MixerPanel.cpp
    components/MixerPanel.h
    components/StatusBar.cpp
    components/StatusBar.h
    components/TransportBar.cpp
    components/TransportBar.h
    theme/DojoLookAndFeel.cpp
    theme/DojoLookAndFeel.h
    theme/DojoTheme.h)
add_library(dojo::ui ALIAS dojo_ui)

target_link_libraries(dojo_ui
    PUBLIC  dojo::engine
    PRIVATE dojo::resources)
dojo_apply_project_warnings(dojo_ui)
set_target_properties(dojo_ui PROPERTIES FOLDER "Dojo")
