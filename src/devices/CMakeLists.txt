add_library(dojo_devices STATIC
    BuiltinProcessor.cpp
    BuiltinProcessor.h
    DeviceRegistry.cpp
    DeviceRegistry.h
    Dynamics.cpp
    Dynamics.h
    Equaliser.cpp
    Equaliser.h
    Instruments.cpp
    Instruments.h
    SampleSlot.cpp
    SampleSlot.h
    TimeEffects.cpp
    TimeEffects.h
    dsp/Biquad.h)
add_library(dojo::devices ALIAS dojo_devices)

target_link_libraries(dojo_devices
    PUBLIC  dojo::core dojo::juce
    PRIVATE dojo::stretch)
dojo_apply_project_warnings(dojo_devices)
set_target_properties(dojo_devices PROPERTIES FOLDER "Dojo")
