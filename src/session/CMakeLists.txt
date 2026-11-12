add_library(dojo_session STATIC
    AudioAssetCache.cpp
    AudioAssetCache.h
    DeviceManager.cpp
    DeviceManager.h
    Exporter.cpp
    Exporter.h
    GraphCompiler.cpp
    GraphCompiler.h
    MediaInfo.cpp
    MediaInfo.h
    Project.cpp
    Project.h
    Recorder.cpp
    Recorder.h
    Session.cpp
    Session.h)
add_library(dojo::session ALIAS dojo_session)

target_link_libraries(dojo_session PUBLIC dojo::engine dojo::devices)
dojo_apply_project_warnings(dojo_session)
set_target_properties(dojo_session PROPERTIES FOLDER "Dojo")
