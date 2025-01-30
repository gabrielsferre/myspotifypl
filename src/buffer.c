typedef struct Buffer {
    u8 *data;
    u64 count;
} Buffer;

#define \
CONSTANT_STRING(string) ((Buffer){(u8*)string, sizeof(string) - 1})

b32
areEqual(Buffer a, Buffer b)
{
    if(a.count != b.count) {
        return 0;
    }
    for(u64 i = 0; i < a.count; ++i) {
        if(a.data[i] != b.data[i]) {
            return 0;
        }
    }
    return 1;
}

Buffer
allocateBuffer(MemoryArena *arena, u64 count)
{
    Buffer buf = {0};
    buf.data = pushArray(arena, count, u8);
    if(buf.data) {
        buf.count = count;
    }
    else {
        check(0 && "unable to allocate buffer");
    }
    return buf;
}

Buffer
copyBuffer(MemoryArena *arena, Buffer buf)
{
    Buffer newBuf = allocateBuffer(arena, buf.count);
    for(u64 i = 0; i < buf.count; ++i) {
        newBuf.data[i] = buf.data[i];
    }
    return newBuf;
}

void
printBuffer(Buffer buf)
{
    if(buf.data) {
        printf("%.*s", (int)buf.count, (char*)buf.data);
    }
}

void
fprintBuffer(FILE *file, Buffer buf) {
    if(buf.data) {
        fprintf(file, "%.*s", (int)buf.count, (char*)buf.data);
    }
}

b32
isInBounds(Buffer buf, u64 offset)
{
    return offset >= 0 && offset < buf.count;
}

Buffer
dumpFileIntoBuffer(MemoryArena *arena, char const *filePath)
{
    FILE *file = fopen(filePath, "r");
    if(!file) {
        fprintf(stderr, "ERROR: Unable to open file \"%s\".\n", filePath);
        check(0);
        return (Buffer){0};
    }
    fseek(file, 0, SEEK_END);
    u64 fileSize = ftell(file) + 1; // maybe the +1 isn't necessary, I'm just making sure 
    fseek(file, 0, SEEK_SET);
    // NOTE: allocating a slightly bigger buffer to avoid annoying off-by-one errors (09/06/2023)
    Buffer buf = allocateBuffer(arena, fileSize + 8);
    if(buf.data) {
        fread(buf.data, sizeof(u8), fileSize, file);
    }
    return buf;
}

Buffer
pushBuffer(MemoryArena *arena, Buffer buf)
{
    Buffer newBuf = {.count = buf.count};
    newBuf.data = pushToMemoryArena(arena, newBuf.count); 
    for(u64 i = 0; i < buf.count; ++i) {
        newBuf.data[i] = buf.data[i];
    }
    return newBuf;
}

Buffer
bufferConcat(MemoryArena *arena, Buffer b1, Buffer b2)
{
    Buffer newBuf = {.count = b1.count + b2.count}; 
    newBuf.data = pushToMemoryArena(arena, newBuf.count);
    u64 newBufIndex = 0;
    for(u64 i = 0; i < b1.count; ++i) {
        u8 value = b1.data[i];
        if(value) {
            newBuf.data[newBufIndex++] = value;
        }
    }
    for(u64 i = 0; i < b2.count; ++i) {
        u8 value = b2.data[i];
        if(value) {
            newBuf.data[newBufIndex++] = value;
        }
    }

    return newBuf;
}

Buffer
bufferConcat5(MemoryArena *arena,
        Buffer b1, Buffer b2, Buffer b3, Buffer b4, Buffer b5)
{
    Buffer newBuf = {
        .count = b1.count + b2.count + b3.count + b4.count + b5.count,
    }; 
    newBuf.data = pushToMemoryArena(arena, newBuf.count);
    u64 newBufIndex = 0;
    for(u64 i = 0; i < b1.count; ++i) {
        u8 value = b1.data[i];
        if(value) {
            newBuf.data[newBufIndex++] = value;
        }
    }
    for(u64 i = 0; i < b2.count; ++i) {
        u8 value = b2.data[i];
        if(value) {
            newBuf.data[newBufIndex++] = value;
        }
    }
    for(u64 i = 0; i < b3.count; ++i) {
        u8 value = b3.data[i];
        if(value) {
            newBuf.data[newBufIndex++] = value;
        }
    }
    for(u64 i = 0; i < b4.count; ++i) {
        u8 value = b4.data[i];
        if(value) {
            newBuf.data[newBufIndex++] = value;
        }
    }
    for(u64 i = 0; i < b5.count; ++i) {
        u8 value = b5.data[i];
        if(value) {
            newBuf.data[newBufIndex++] = value;
        }
    }

    return newBuf;
}
