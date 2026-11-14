add_library(dojo_engine STATIC
    AudioEngine.cpp
    AudioEngine.h
    ChannelLayout.cpp
    ChannelLayout.h
    Automation.cpp
    Automation.h
    Capture.h
    EngineCommands.h
    EngineController.cpp
    EngineController.h
    EngineTelemetry.h
    Preview.cpp
    Preview.h
    ProcessorNode.cpp
    ProcessorNode.h
    RealtimeHandoff.cpp
    RealtimeHandoff.h
    RenderGraph.h
    WorkerPool.cpp
    WorkerPool.h
    dsp/ClipRenderer.cpp
    dsp/ClipRenderer.h
    dsp/ClipStretcher.cpp
    dsp/ClipStretcher.h
    dsp/SurroundVirtualizer.cpp
    dsp/SurroundVirtualizer.h
    dsp/TestToneGenerator.cpp
    dsp/TestToneGenerator.h
    io/AudioDeviceHost.cpp
    io/AudioDeviceHost.h
    io/LameMp3Format.cpp
    io/LameMp3Format.h)
add_library(dojo::engine ALIAS dojo_engine)

target_link_libraries(dojo_engine
    PUBLIC  dojo::core dojo::juce
    PRIVATE dojo::stretch dojo::lame)
dojo_apply_project_warnings(dojo_engine)
set_target_properties(dojo_engine PROPERTIES FOLDER "Dojo")
