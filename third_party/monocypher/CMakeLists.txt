# Monocypher 4.0.2 (CC0-1.0 or BSD-2-Clause, see LICENCE.md in this folder): Ed25519 (SHA-512)
# signatures for license keys, BLAKE2b keyed hashes. Vendored verbatim from monocypher-4.0.2.tar.gz
# (monocypher.org, SHA-512 bf275d4c53ff94af6cdc723a4e002e9f080f4d1436c86c76bb37870b34807f1d7b32331d8ff8a1aeb369e946f3769021e03e63efac25b82efc5abf54dc084714):
# src/monocypher.{c,h} and src/optional/monocypher-ed25519.{c,h}.

add_library(dojo_monocypher STATIC monocypher.c monocypher-ed25519.c)
add_library(dojo::monocypher ALIAS dojo_monocypher)
target_include_directories(dojo_monocypher SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(dojo_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "ThirdParty" C_STANDARD 99)

# Third-party code: compiled quietly (see the JUCE note in cmake/DojoDependencies.cmake).
if(MSVC)
    target_compile_options(dojo_monocypher PRIVATE /W0)
else()
    target_compile_options(dojo_monocypher PRIVATE -w)
endif()
