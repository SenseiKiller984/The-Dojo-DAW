# Embedded resources. Fonts are compiled into the binary so that text renders with identical
# metrics on Windows and Linux (system UI fonts differ: Segoe UI vs. whatever fontconfig picks),
# which keeps every layout pixel-identical across platforms.
#
# Built-in device presets and chains (presets/*.dojopreset) are compiled in too: every
# ".dojopreset" listed here shows up as a built-in preset (session/Presets.h).
#
# Built-in drum kits: presets/kits/*.dojopreset (Drum Rack presets with a demo beat) and the
# drum machines' samples, one archive per machine (kits/<Machine>.zip, unpacked into the
# library folder when first used: devices/DrumKits.h). Both are made by
# tools/drumkits/make_kits.py from the original sample sets.
#
# The Roland and Akai machines' samples (127 MB of WAV) are too big to compile in: their archives
# (drum-machines/<Machine>.zip) are copied beside the application into "Drum Machines" (and beside
# the test runner), where devices/DrumKits.h finds them: dojo_copy_drum_machines(<target>).
#
# Licences: both families are SIL Open Font License 1.1 — see fonts/LICENSE-*.txt.

file(GLOB DOJO_KIT_PRESETS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/kits/*.dojopreset")
file(GLOB DOJO_EMBEDDED_KITS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kits/*.zip")

juce_add_binary_data(dojo_resources
    NAMESPACE   DojoBinaryData
    HEADER_NAME DojoBinaryData.h
    SOURCES
        fonts/Inter-Regular.ttf
        fonts/Inter-SemiBold.ttf
        fonts/JetBrainsMono-Medium.ttf
        "presets/Synth Bite.dojopreset"
        "presets/Punchy Drum Bus.dojopreset"
        "presets/Dusty Lo-Fi Drums.dojopreset"
        "presets/Pop Vocal Polish.dojopreset"
        "presets/Upfront Rap Vocal.dojopreset"
        "presets/Drivers License Vocal Saturation.dojopreset"
        "presets/Dreamy Wide Space.dojopreset"
        "presets/Old Radio.dojopreset"
        "presets/Gritty Bass Weight.dojopreset"
        "presets/Shimmer Bloom.dojopreset"
        ${DOJO_KIT_PRESETS}
        ${DOJO_EMBEDDED_KITS})

add_library(dojo::resources ALIAS dojo_resources)
set_target_properties(dojo_resources PROPERTIES FOLDER "Resources")

file(GLOB DOJO_DRUM_MACHINE_ARCHIVES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/drum-machines/*.zip")
set(DOJO_DRUM_MACHINE_ARCHIVES "${DOJO_DRUM_MACHINE_ARCHIVES}" CACHE INTERNAL "The drum machines' sample archives shipped beside the application")

# Copies the drum machines' archives into "Drum Machines" beside `target`'s binary after each build.
function(dojo_copy_drum_machines target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/Drum Machines"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DOJO_DRUM_MACHINE_ARCHIVES} "$<TARGET_FILE_DIR:${target}>/Drum Machines"
        VERBATIM)
endfunction()
