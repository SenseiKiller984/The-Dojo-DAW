#include "app/Hardening.h"

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace dojo::app::hardening
{

void applyProcessHardening() noexcept
{
#if defined(_WIN32)
    // Only the application directory, System32 and explicitly added directories are searched for
    // DLLs: never the current directory or %PATH%. Blocks classic DLL-planting hijacks.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    SetDllDirectoryW(L"");

    // Do not load DLLs from remote (network) locations, and prefer the copies in System32.
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY imageLoad{};
    imageLoad.NoRemoteImages = 1;
    imageLoad.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &imageLoad, sizeof(imageLoad));
#endif
}

} // namespace dojo::app::hardening
