add_library(dojo_engine STATIC
    AudioEngine.cpp
    AudioEngine.h
    EngineCommands.h
    EngineController.cpp
    EngineController.h
    EngineTelemetry.h
    dsp/TestToneGenerator.cpp
    dsp/TestToneGenerator.h
    io/AudioDeviceHost.cpp
    io/AudioDeviceHost.h)
add_library(dojo::engine ALIAS dojo_engine)

target_link_libraries(dojo_engine PUBLIC dojo::core dojo::juce)
dojo_apply_project_warnings(dojo_engine)
set_target_properties(dojo_engine PROPERTIES FOLDER "Dojo")
