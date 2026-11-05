# LAME 3.100 MP3 encoder (LGPL-2.0-or-later; see COPYING and LICENSE in this folder).
# Vendored verbatim from lame-3.100.tar.gz (SHA-256 ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e):
# only libmp3lame/ and include/lame.h are kept. config.h is ours.

file(GLOB _lame_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libmp3lame/*.c")

add_library(dojo_lame STATIC ${_lame_sources})
add_library(dojo::lame ALIAS dojo_lame)

target_include_directories(dojo_lame
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/libmp3lame")
target_compile_definitions(dojo_lame PRIVATE HAVE_CONFIG_H)
set_target_properties(dojo_lame PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "ThirdParty" C_STANDARD 99)

# Third-party code: compiled quietly (see the JUCE note in cmake/DojoDependencies.cmake).
if(MSVC)
    target_compile_options(dojo_lame PRIVATE /W0)
    target_compile_definitions(dojo_lame PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(dojo_lame PRIVATE -w)
    target_link_libraries(dojo_lame PRIVATE m)
endif()
