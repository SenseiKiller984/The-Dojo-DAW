// Real-time allocation trap.
//
// Replaces the global operator new / operator delete family with thin wrappers around malloc/free
// that first check whether the calling thread is inside a dojo::rt::ScopedRealtimeSection (i.e. is
// running the audio callback). If it is, the allocation is still served — we never want a debug
// aid to crash a performance — but it is counted as a real-time violation. The count is surfaced in
// the status bar of development builds and asserted to be zero by the test-suite.
//
// This file must be linked directly into an executable (see the dojo_allocation_trap OBJECT library);
// replacement allocation functions are a whole-program property.

#include "core/rt/RealtimeGuard.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h> // _aligned_malloc / _aligned_free
#endif

namespace
{
inline void noteHeapActivity() noexcept
{
    if (dojo::rt::isInRealtimeSection())
        dojo::rt::reportRealtimeViolation();
}

void* allocate(std::size_t size)
{
    noteHeapActivity();

    if (size == 0)
        size = 1;

    for (;;)
    {
        if (void* ptr = std::malloc(size))
            return ptr;

        if (auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}

void* allocateNoThrow(std::size_t size) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    noteHeapActivity();

    if (size == 0)
        size = 1;

    const auto align = std::max(static_cast<std::size_t>(alignment), sizeof(void*));

    for (;;)
    {
#if defined(_WIN32)
        if (void* ptr = _aligned_malloc(size, align))
            return ptr;
#else
        void* ptr = nullptr;
        if (posix_memalign(&ptr, align, size) == 0)
            return ptr;
#endif

        if (auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}

void* allocateAlignedNoThrow(std::size_t size, std::align_val_t alignment) noexcept
{
    try
    {
        return allocateAligned(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}

void deallocate(void* ptr) noexcept
{
    if (ptr == nullptr)
        return;

    noteHeapActivity();
    std::free(ptr);
}

void deallocateAligned(void* ptr) noexcept
{
    if (ptr == nullptr)
        return;

    noteHeapActivity();
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

// Lets the rest of the program discover (at run time) that the trap is active.
[[maybe_unused]] const bool trapRegistered = []
{
    dojo::rt::detail::markAllocationTrapInstalled();
    return true;
}();
} // namespace

// clang-format off
void* operator new  (std::size_t size)                                   { return allocate(size); }
void* operator new[](std::size_t size)                                   { return allocate(size); }
void* operator new  (std::size_t size, const std::nothrow_t&) noexcept   { return allocateNoThrow(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept   { return allocateNoThrow(size); }

void* operator new  (std::size_t size, std::align_val_t al)                         { return allocateAligned(size, al); }
void* operator new[](std::size_t size, std::align_val_t al)                         { return allocateAligned(size, al); }
void* operator new  (std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return allocateAlignedNoThrow(size, al); }
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return allocateAlignedNoThrow(size, al); }

void operator delete  (void* ptr) noexcept                               { deallocate(ptr); }
void operator delete[](void* ptr) noexcept                               { deallocate(ptr); }
void operator delete  (void* ptr, const std::nothrow_t&) noexcept        { deallocate(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept        { deallocate(ptr); }

void operator delete  (void* ptr, std::align_val_t) noexcept                         { deallocateAligned(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept                         { deallocateAligned(ptr); }
void operator delete  (void* ptr, std::align_val_t, const std::nothrow_t&) noexcept  { deallocateAligned(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept  { deallocateAligned(ptr); }

// Sized deallocation (C++14) is on by default in GCC and MSVC but only from Clang 19 onwards; the
// sized overloads are declared - and called by the compiler - only when it is enabled. When it is
// off, every delete expression already lands in the unsized overloads above.
#if defined(__cpp_sized_deallocation) || defined(_MSC_VER)
void operator delete  (void* ptr, std::size_t) noexcept                              { deallocate(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept                              { deallocate(ptr); }
void operator delete  (void* ptr, std::size_t, std::align_val_t) noexcept            { deallocateAligned(ptr); }
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept            { deallocateAligned(ptr); }
#endif
// clang-format on
