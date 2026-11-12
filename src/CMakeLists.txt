# Layering (each layer may only depend on the ones above it):
#
#   core    pure C++20, no JUCE: real-time primitives (lock-free queues, RT guards), shared types
#   engine  audio engine, device host, command/telemetry protocol (JUCE audio modules)
#   plugins plug-in hosting: VST3 (JUCE) + CLAP (own host), out-of-process scanning (+ scanner exe)
#   devices native devices: EQ, dynamics, reverb, delay, pitch, Sampler, Drum Rack (juce::AudioProcessor)
#   session the document: tracks and clips (ValueTree + undo), decoded audio, render-graph compiler
#   video   video decoding (built-in Motion-JPEG AVI, FFmpeg, Media Foundation) for video tracks
#   ui      look-and-feel and GUI components (JUCE GUI modules); talks to the engine only through
#           dojo::engine::EngineController
#   app     JUCEApplication entry point and main window

add_subdirectory(core)
add_subdirectory(engine)
add_subdirectory(devices)
add_subdirectory(plugins)
add_subdirectory(scanner)
add_subdirectory(session)
add_subdirectory(video)
add_subdirectory(ui)
add_subdirectory(app)
