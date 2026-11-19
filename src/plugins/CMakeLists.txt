add_library(dojo_plugins STATIC
    ClapHosting.cpp
    ClapHosting.h
    PluginManager.cpp
    PluginManager.h)
add_library(dojo::plugins ALIAS dojo_plugins)

target_link_libraries(dojo_plugins
    PUBLIC  dojo::core dojo::juce
    PRIVATE dojo::clap)
dojo_apply_project_warnings(dojo_plugins)
set_target_properties(dojo_plugins PROPERTIES FOLDER "Dojo")
