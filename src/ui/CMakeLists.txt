add_library(dojo_ui STATIC
    AppServices.h
    BrowserDrag.h
    Commands.h
    DemoProject.cpp
    DemoProject.h
    DeviceActions.cpp
    DeviceActions.h
    EditActions.cpp
    EditActions.h
    EditorState.cpp
    EditorState.h
    MainComponent.cpp
    MainComponent.h
    MainMenuModel.cpp
    MainMenuModel.h
    components/ArrangerView.cpp
    components/ArrangerView.h
    components/BrowserPanel.cpp
    components/BrowserPanel.h
    components/ClipEditor.cpp
    components/ClipEditor.h
    components/DevicePanel.cpp
    components/DevicePanel.h
    components/ExportDialog.cpp
    components/ExportDialog.h
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
    devices/DeviceEditors.cpp
    devices/DeviceEditors.h
    devices/ParameterControl.cpp
    devices/ParameterControl.h
    theme/DojoLookAndFeel.cpp
    theme/DojoLookAndFeel.h
    theme/DojoTheme.h)
add_library(dojo::ui ALIAS dojo_ui)

target_link_libraries(dojo_ui
    PUBLIC  dojo::session dojo::plugins
    PRIVATE dojo::resources)
dojo_apply_project_warnings(dojo_ui)
set_target_properties(dojo_ui PROPERTIES FOLDER "Dojo")
