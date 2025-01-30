#define RESERVED_BYTE_COUNT (1ull << 36) 
#define GUARD_SPACE_PAGE_COUNT 2

typedef struct MemoryArena {
    u8 *data;
    u64 count;
    u64 maxCount;
} MemoryArena;

static MemoryArena
allocateMemoryArena(u64 byteCount)
{
    MemoryArena arena = {0};
    u64 pageByteCount = getVirtualPageByteCount();
    u64 guardSpaceByteCount = GUARD_SPACE_PAGE_COUNT * pageByteCount;
    b32 enoughSpace = (RESERVED_BYTE_COUNT - guardSpaceByteCount >= byteCount);
    if(enoughSpace) {
        u8 *begin = reserveVirtualMemory(RESERVED_BYTE_COUNT);
        u8 *guardPage = begin;
        u8 *arenaData = guardPage + pageByteCount;
        b32 error = mapReservedMemory(arenaData, byteCount);
        if(!error) {
            arena = (MemoryArena){.data = arenaData, .maxCount = byteCount};
        }
        else {
            check(0 && "couldn't map memory");
            panic(0, "not enought memory");
        }
    }
    else {
        check(0 && "not enough space");
        panic(0, "not enought memory");
    }
    return arena;
}

static b32
growMemoryArena(MemoryArena *arena, u64 count)
{
    b32 ret = 0;
    u64 guardSpaceByteCount =
        GUARD_SPACE_PAGE_COUNT * getVirtualPageByteCount();
    b32 enoughMemory = count < RESERVED_BYTE_COUNT - guardSpaceByteCount;
    if(enoughMemory) {
        b32 error = mapReservedMemory(arena->data, count);
        if(error) {
            check(0 && "couldn't map virtual memory");
            panic(0, "not enought memory");
        }
        arena->maxCount = (count > arena->maxCount) ? count : arena->maxCount;
    }
    else {
        check(0 && "not enought memory to grow arena");
        panic(0, "not enought memory");
        ret = 1;
    }
    return ret;
}

static void
freeMemoryArena(MemoryArena *arena)
{
    u64 pageByteCount = getVirtualPageByteCount();
    u8 *guardPage = arena->data - pageByteCount;
    b32 error = freeVirtualMemory(guardPage, RESERVED_BYTE_COUNT);
    if(error) {
        check(0 && "couldn't free virtual memory");
    }
}

static u8* 
pushToMemoryArena(MemoryArena *arena, u64 count)
{
    u8 *reservedData = 0;
    b32 enoughMemory = (arena->maxCount - arena->count >= count);
    if(!enoughMemory) {
        u64 defaultCount = 2 * arena->count;
        u64 requiredCount = arena->count + count;
        u64 newMax = (defaultCount > requiredCount) ?
            defaultCount : requiredCount;
        enoughMemory = !growMemoryArena(arena, newMax);
    }
    if(enoughMemory) {
        reservedData = arena->data + arena->count;
        arena->count += count;
    }
    return reservedData;
}

static void
popFromMemoryArena(MemoryArena *arena, u64 count)
{
    b32 enoughMemory = (arena->count >= count);
    if(!enoughMemory) {
        check("popping more memory than it's avaiable in arena");
    }
    count = enoughMemory ? count : arena->count;
    arena->count -= count;
    u8 *base = arena->data + arena->count;
    for(u64 i = 0; i < count; ++i) {
        base[i] = 0;
    }
}

static void
clearMemoryArena(MemoryArena *arena)
{
    popFromMemoryArena(arena, arena->count);
}

#define \
pushStruct(arena, type) \
    ((type*)pushToMemoryArena((arena),(sizeof(type))))

#define \
pushArray(arena, count, type) \
    ((type*)pushToMemoryArena((arena),(count)*sizeof(type)))

