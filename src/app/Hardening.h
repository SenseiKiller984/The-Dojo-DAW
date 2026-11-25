#pragma once

namespace dojo::app::hardening
{

/**
    Process hardening applied once at start-up, before plug-ins or other libraries are loaded.

    On Windows it tightens the DLL search path (so a DLL planted next to the executable or in the
    current directory cannot be loaded in place of a system one) and sets image-load mitigations
    (no DLLs from remote drives, prefer System32). On Linux it does nothing at run time; the
    exploit mitigations there are applied at build time (DOJO_HARDENING).

    These stop DLL-search-order hijacking and some injection vectors; they cannot stop an attacker
    who already runs code at the same or higher privilege.
*/
void applyProcessHardening() noexcept;

} // namespace dojo::app::hardening
