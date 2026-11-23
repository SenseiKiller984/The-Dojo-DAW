add_library(dojo_session STATIC
    AdmOscBridge.cpp
    AdmOscBridge.h
    AraHost.cpp
    AraHost.h
    audioedit/PitchAnalysis.cpp
    audioedit/PitchAnalysis.h
    audioedit/PitchRender.cpp
    audioedit/PitchRender.h
    audioedit/Spectral.cpp
    audioedit/Spectral.h
    audioedit/Stft.cpp
    audioedit/Stft.h
    AudioAssetCache.cpp
    AudioAssetCache.h
    AudioEdits.cpp
    AudioEdits.h
    DeviceManager.cpp
    DeviceManager.h
    Exporter.cpp
    Exporter.h
    GraphCompiler.cpp
    GraphCompiler.h
    MediaInfo.cpp
    MediaInfo.h
    Presets.cpp
    Presets.h
    Project.cpp
    Project.h
    Recorder.cpp
    Recorder.h
    Session.cpp
    Session.h
    Takes.cpp
    TempoDetector.cpp
    TempoDetector.h)
add_library(dojo::session ALIAS dojo_session)

target_link_libraries(dojo_session PUBLIC dojo::engine dojo::devices dojo::video PRIVATE dojo::resources)
dojo_apply_project_warnings(dojo_session)
set_target_properties(dojo_session PROPERTIES FOLDER "Dojo")
