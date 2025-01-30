#if defined(__APPLE__) || defined(__MACH__)
#define MAC_OS 1
#else
#define MAC_OS 0
#endif

#if defined(__linux__)
#define LINUX 1
#else
#define LINUX 0
#endif

#if defined(__FreeBSD__)
#define FREE_BSD 1
#else
#define FREE_BSD 0
#endif

#if LINUX || FREE_BSD || MAC_OS
#define UNIX 1
#else
#define UNIX 0
#endif

#if UNIX

#include <unistd.h>
#include <sys/mman.h>

u64
getVirtualPageByteCount()
{
    u64 count = getpagesize();
    return count;
}

u8*
reserveVirtualMemory(u64 byteCount)
{
    u8 *data =
        mmap(0, byteCount, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0);
    return data;
}

b32
mapReservedMemory(u8 *data, u64 byteCount)
{
    int error = mprotect(data, byteCount, PROT_READ|PROT_WRITE);
    return error ? 1 : 0;
}

b32
freeVirtualMemory(u8 *data, u64 byteCount)
{
    int error = munmap(data, byteCount);
    return error ? 1 : 0;
}

#else
static_assert("operating system not supported");
#endif

#undef MAC_OS
#undef UNIX
#undef LINUX
#undef FREE_BSD
