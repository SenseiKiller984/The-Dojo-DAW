juce_add_gui_app(TheDojo
    PRODUCT_NAME "TheDojo"
    COMPANY_NAME "The Dojo Project"
    BUNDLE_ID    "io.github.thedojo.daw"
    VERSION      ${PROJECT_VERSION})

target_sources(TheDojo PRIVATE
    CommandLineOptions.cpp
    CommandLineOptions.h
    DojoApplication.cpp
    DojoApplication.h
    Main.cpp
    MainWindow.cpp
    MainWindow.h
    SmokeTest.cpp
    SmokeTest.h)

# Development builds carry the real-time allocation trap (see src/core/rt/AllocationTrap.cpp);
# the violation counter is shown in the status bar.
if(DOJO_RT_ALLOCATION_TRAP AND DOJO_ALLOCATION_TRAP_COMPATIBLE)
    target_sources(TheDojo PRIVATE $<$<NOT:$<CONFIG:Release>>:$<TARGET_OBJECTS:dojo_allocation_trap>>)
endif()

target_link_libraries(TheDojo PRIVATE dojo::ui dojo::plugins)

# The out-of-process plug-in scanner lives next to the application binary.
add_dependencies(TheDojo dojo-plugin-scanner)
add_custom_command(TARGET TheDojo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:dojo-plugin-scanner> $<TARGET_FILE_DIR:TheDojo>
    VERBATIM)
dojo_apply_project_warnings(TheDojo)

set_target_properties(TheDojo PROPERTIES FOLDER "Dojo")
