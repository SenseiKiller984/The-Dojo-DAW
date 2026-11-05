# Layering (each layer may only depend on the ones above it):
#
#   core    pure C++20, no JUCE: real-time primitives (lock-free queues, RT guards), shared types
#   engine  audio engine, device host, command/telemetry protocol (JUCE audio modules)
#   devices native devices: EQ, dynamics, reverb, delay, pitch, Sampler, Drum Rack (juce::AudioProcessor)
#   session the document: tracks and clips (ValueTree + undo), decoded audio, render-graph compiler
#   ui      look-and-feel and GUI components (JUCE GUI modules); talks to the engine only through
#           dojo::engine::EngineController
#   app     JUCEApplication entry point and main window

add_subdirectory(core)
add_subdirectory(engine)
add_subdirectory(devices)
add_subdirectory(session)
add_subdirectory(ui)
add_subdirectory(app)
