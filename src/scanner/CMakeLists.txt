# The out-of-process plug-in scanner (see PluginManager). Installed next to the application.
add_executable(dojo-plugin-scanner ScannerMain.cpp)
target_link_libraries(dojo-plugin-scanner PRIVATE dojo::plugins)
dojo_apply_project_warnings(dojo-plugin-scanner)
set_target_properties(dojo-plugin-scanner PROPERTIES FOLDER "Dojo")
