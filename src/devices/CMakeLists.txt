add_library(dojo_devices STATIC
    AmpCabinet.cpp
    AmpCabinet.h
    BuiltinProcessor.cpp
    BuiltinProcessor.h
    DeviceRegistry.cpp
    DeviceRegistry.h
    Dynamics.cpp
    Dynamics.h
    Echo.cpp
    Echo.h
    Equaliser.cpp
    Equaliser.h
    Impulse.cpp
    Impulse.h
    Instruments.cpp
    Instruments.h
    Operator.cpp
    Operator.h
    SampleSlot.cpp
    SampleSlot.h
    TimeEffects.cpp
    TimeEffects.h
    Utility.cpp
    Utility.h
    WavetableSynth.cpp
    WavetableSynth.h
    dsp/Biquad.h
    dsp/SynthCore.cpp
    dsp/SynthCore.h)
add_library(dojo::devices ALIAS dojo_devices)

target_link_libraries(dojo_devices
    PUBLIC  dojo::core dojo::juce
    PRIVATE dojo::stretch)
dojo_apply_project_warnings(dojo_devices)
set_target_properties(dojo_devices PROPERTIES FOLDER "Dojo")
