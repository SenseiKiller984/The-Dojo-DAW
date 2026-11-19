# Language standard, warning policy and sanitizer plumbing shared by every Dojo target.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(DOJO_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _dojo_ipo_supported OUTPUT _dojo_ipo_message)
    if(_dojo_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else()
        message(WARNING "LTO requested but not supported: ${_dojo_ipo_message}")
    endif()
endif()

# --------------------------------------------------------------------------------------------------
# dojo::sanitizers — instrumentation flags, linked PUBLIC into Dojo libraries so that every object
# in an executable agrees on the instrumentation.
add_library(dojo_sanitizers INTERFACE)
add_library(dojo::sanitizers ALIAS dojo_sanitizers)

if(DOJO_SANITIZER)
    if(MSVC)
        if(DOJO_SANITIZER STREQUAL "address")
            target_compile_options(dojo_sanitizers INTERFACE /fsanitize=address)
        else()
            message(FATAL_ERROR "MSVC only supports DOJO_SANITIZER=address")
        endif()
    else()
        set(_dojo_san_flags -fsanitize=${DOJO_SANITIZER} -fno-omit-frame-pointer)
        if(DOJO_SANITIZER STREQUAL "undefined")
            list(APPEND _dojo_san_flags -fno-sanitize-recover=undefined)
        endif()
        target_compile_options(dojo_sanitizers INTERFACE ${_dojo_san_flags})
        target_link_options(dojo_sanitizers INTERFACE -fsanitize=${DOJO_SANITIZER})
    endif()
endif()

# --------------------------------------------------------------------------------------------------
# dojo_apply_project_warnings(<target>)
# Applies JUCE's recommended (strict) warning set — -Wconversion, -Wsign-conversion, -Wshadow,
# -Wfloat-equal, -Wswitch-enum ... on GCC/Clang, /W4 on MSVC — privately to a Dojo target.
# Third-party code (JUCE modules, Catch2) is never compiled with these flags.
function(dojo_apply_project_warnings target)
    target_link_libraries(${target} PRIVATE juce::juce_recommended_warning_flags)

    if(MSVC)
        # C4324: "structure was padded due to alignment specifier" - intentional for cache-line
        # isolation in the lock-free queues, and reported for every class that contains one.
        target_compile_options(${target} PRIVATE /permissive- /utf-8 /Zc:__cplusplus /wd4324)
    endif()

    if(DOJO_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /WX)
        else()
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
