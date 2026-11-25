add_library(dojo_licensing STATIC
    CaptureGuard.cpp
    CaptureGuard.h
    Crypto.cpp
    Crypto.h
    DemoLedger.cpp
    DemoLedger.h
    Features.cpp
    Features.h
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
dojo_apply_project_warnings(dojo_licensing)
set_target_properties(dojo_licensing PROPERTIES FOLDER "Dojo")
