add_library(dojo_licensing STATIC
    CaptureGuard.cpp
    CaptureGuard.h
    Crypto.cpp
    Crypto.h
    DemoLedger.cpp
    DemoLedger.h
    Features.cpp
    Features.h
    Integrity.cpp
    Integrity.h
    Obfuscation.h
    LicenseKey.cpp
    LicenseKey.h
    LicenseManager.cpp
    LicenseManager.h
    PublicKey.cpp
    PublicKey.h)
add_library(dojo::licensing ALIAS dojo_licensing)

target_link_libraries(dojo_licensing
    PUBLIC  dojo::core dojo::juce
    PRIVATE dojo::monocypher)
target_compile_definitions(dojo_licensing PUBLIC DOJO_TAMPER_CHECK=$<BOOL:${DOJO_TAMPER_CHECK}>)
dojo_apply_project_warnings(dojo_licensing)
set_target_properties(dojo_licensing PROPERTIES FOLDER "Dojo")
