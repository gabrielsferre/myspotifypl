// TODO:
//   - Improve error messages.
//   - Support more operating systems.
//   - Do some profiling to check if performance could get substantially better.
//   - Improve memory management.
//   - Remove the libcurl dependency.

#include "includes.c"

#include <curl/curl.h>
#include "config.c"

#define \
AUTHORIZATION_CODE_ACCESS_URI "https://accounts.spotify.com/authorize?client_id="CLIENT_ID"&response_type=code&redirect_uri="REDIRECT_URI"&scope=playlist-read-collaborative"
#define \
TOKEN_URI "https://accounts.spotify.com/api/token/"
#define \
PLAYLIST_URI "https://api.spotify.com/v1/playlists/"
#define \
PLAYLIST_LIST_URI "https://api.spotify.com/v1/me/playlists/"
#define \
OK_RESPONSE 200
#define \
EXPIRED_TOKEN_RESPONSE 401
#define \
CONNECTION_COUNT 128

#define MEGABYTE (1ull << 20)

#define \
CS(str) CONSTANT_STRING(str)

#define \
errorAndTerminate(format, ...) \
({ \
    fprintf(stderr, "Error: "format"\n"__VA_OPT__(,) __VA_ARGS__); \
    check(0); \
    exit(1); \
})

#define \
printWarning(format, ...) \
({ \
    fprintf(stderr, "Warning: "format"\n"__VA_OPT__(,) __VA_ARGS__); \
    check(0); \
})

typedef struct Track {
    Buffer title;
    Buffer album;
    Buffer dateAdded;
    u64 durationInMs;
    Buffer *artistArray;
    u64 artistCount;
} Track;

typedef struct Playlist {
    Buffer name;
    Track *trackArray;
    u64 trackCount;
    u64 filledTrackCount;
} Playlist;

typedef struct PlaylistArray {
    Playlist *data;
    u64 count;
} PlaylistArray;

typedef enum JobType {
    Job_zero,
    Job_playlistListHeader,
    Job_playlistList,
    Job_playlistHeader,
    Job_trackList,
} JobType;

typedef struct Job {
    JobType type;
    Buffer uri;
    json_Element json;
    u64 playlistIndex;
    u64 offset;
} Job;

typedef struct JobQueue {
    Job *data;
    u64 maxCount;
    u64 count;
    u64 first;
    u64 last;
    MemoryArena *arena;
} JobQueue;

typedef struct NetworkState {
    CURLM *multiHandle;
    u64 easyHandleCount;
    CURL **easyHandleArray;
    Job *handleToJobMap;
    MemoryArena *handleToArenaMap;
    b32 *busyHandleFlagArray;
    u64 busyHandleCount;
    Buffer accessToken;
    Buffer refreshToken;
} NetworkState;

typedef struct AppMemory {
    MemoryArena curlBuffer;
    MemoryArena persistent;
    MemoryArena scratch;
} AppMemory;

typedef struct State {
    JobQueue jobQueue;
    PlaylistArray playlistArray;
    AppMemory memory;
    NetworkState networkState;
} State;

static void
growJobQueue(JobQueue *jq)
{
    u64 newMaxCount = 2 * jq->maxCount;
    Job *newData = pushArray(jq->arena, newMaxCount, Job);
    check(newData);
    u64 newQueueIndex = 0;
    for(u64 i = jq->first, count = 0; count < jq->count; ++i, ++count) {
        if(i >= jq->maxCount) {
            i = 0;
        }
        newData[newQueueIndex++] = jq->data[i];
    }
    jq->first = 0;
    jq->last = newQueueIndex;
    jq->data = newData;
    jq->maxCount = newMaxCount;
}

static void
enqueueJob(JobQueue *jq, Job job)
{
    b32 queueIsFull = jq->count + 1 > jq->maxCount;
    if(queueIsFull) {
        growJobQueue(jq);
    }
    jq->data[jq->last] = job;
    jq->last = (jq->maxCount) ? (jq->last + 1) % jq->maxCount : 0;
    jq->count += 1;
}

static Job
dequeueJob(JobQueue *jq)
{
    Job job = {0};
    if(jq->count) {
        job = jq->data[jq->first];
        jq->data[jq->first] = (Job){0};
        jq->first = (jq->maxCount) ? (jq->first + 1) % jq->maxCount : 0;
        jq->count -= 1;
    }
    return job;
}

static void
initJobQueue(JobQueue *queue, u64 count, MemoryArena *arena)
{
    queue->data = pushArray(arena, count, Job);
    queue->maxCount = count;
    queue->arena = arena;
}

static b32
isJobQueueEmpty(JobQueue const *jq)
{
    return jq->count == 0;
}

static void
deinit(State *st)
{
    // deinit libcurl
    {
        curl_global_cleanup();
    }
    for(u64 i = 0; i < st->playlistArray.count; ++i) {
        Playlist playlist = st->playlistArray.data[i];
        check(playlist.filledTrackCount == playlist.trackCount);
    }
    freeMemoryArena(&st->memory.curlBuffer);
    freeMemoryArena(&st->memory.persistent);
    freeMemoryArena(&st->memory.scratch);
}

typedef MemoryArena CallbackArgument;

static u64
writeDataLibcurlCallback(void *buffer, u64 membsize, u64 nmemb, void *userp)
{
    CallbackArgument *arena = (CallbackArgument*)userp;
    u64 writeCount = membsize*nmemb;
    u8 *data = pushToMemoryArena(arena, writeCount); 
    memcpy(data, buffer, writeCount);
    return writeCount;
}

static void
initLibcurl(State *st)
{
    // TODO: use curl_version_info to check if used features are available
    curl_global_init(CURL_GLOBAL_ALL);
}

static void
clearCurlBufferAndSendRequest(CURL *handle, MemoryArena *handleArena)
{
    // NOTE: it doesn't seem necessary to clear the buffer every time, if performance is an issue we should be able to keep it dirty.
    clearMemoryArena(handleArena);
    {
        curl_easy_perform(handle);
    }
}

static void
httpPostToken(CURL *handle, MemoryArena *handleArena, Buffer postStr)
{
    curl_easy_setopt(handle,CURLOPT_POSTFIELDS, postStr.data);
    curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(handle, CURLOPT_USERPWD, CLIENT_ID":"CLIENT_SECRET);
    curl_easy_setopt(handle, CURLOPT_URL, TOKEN_URI);
    curl_easy_setopt(handle, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeDataLibcurlCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, handleArena);
    clearCurlBufferAndSendRequest(handle, handleArena);

    long responseCode = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &responseCode);

    if(responseCode != OK_RESPONSE) {
        errorAndTerminate("problem while connecting with spotify, "
                "check if you copied the authorization code correctly");
    }
}

Buffer
u64ToString(MemoryArena *arena, u64 number)
{
    u64 bufferCount = 1;
    u64 x = number/10;
    while(x != 0) {
        x /= 10;
        bufferCount += 1;
    }
    Buffer buf = allocateBuffer(arena, bufferCount);
    u64 y = number;
    for(u64 i = 0; i < bufferCount; ++i) {
        u64 r = y % 10;
        y /= 10;
        buf.data[bufferCount - i - 1] = (u8)(r + '0');
    }
    return buf;
}

static Buffer
pushBufferAsCString(MemoryArena *arena, Buffer buf)
{
    Buffer newBuf = {.count = buf.count + 1};
    newBuf.data = pushToMemoryArena(arena, newBuf.count);
    for(u64 i = 0; i < buf.count; ++i) {
        newBuf.data[i] = buf.data[i];
    }
    return newBuf;
}

static Buffer
copyString(MemoryArena *arena, json_Element element, Buffer fieldName)
{
    json_Element strElement = json_getElement(element, fieldName);
    check(strElement.type == json_STRING ||
            strElement.type == json_INVALID_ELEMENT);
    Buffer newBuf = {0};
    if(strElement.type) {
        newBuf = pushBuffer(arena, strElement.value);
    }
    return newBuf;
}

static Buffer
copyStringAndEscapeCommas(
        MemoryArena *arena, json_Element element, Buffer fieldName)
{
    json_Element strElement = json_getElement(element, fieldName);
    check(strElement.type == json_STRING ||
            strElement.type == json_INVALID_ELEMENT);
    Buffer newBuf = {0};
    if(strElement.type == json_STRING) {
        Buffer oldBuf = strElement.value;
        u64 commaCount = 0;
        for(u64 i = 0; i < oldBuf.count; ++i) {
            char ch = (char)oldBuf.data[i];
            if(ch == '"') {
                commaCount += 1;
            }
        }
        newBuf = allocateBuffer(arena, oldBuf.count + commaCount);
        u64 newIndex = 0;
        for(u64 oldIndex = 0; oldIndex < oldBuf.count; oldIndex += 1) {
            check(newIndex < newBuf.count);
            u8 ch = oldBuf.data[oldIndex];
            newBuf.data[newIndex++] = ch;
            if(ch == (u8)'"') {
                newBuf.data[newIndex++] = '"';
            }
        }
        check(newIndex == newBuf.count);
    }
    return newBuf;
}

static json_Element
parseBufferToJson(MemoryArena *jsonArena, Buffer text)
{
    json_Element *jsonRoot = json_parseJson(jsonArena, text);
    if(!jsonRoot->type) {
        errorAndTerminate("couldn't read spotify response");
    }
    return *jsonRoot;
}

static void
getAccessTokensFromJson(
        NetworkState *nst, AppMemory *memory, json_Element tokenJson)
{
    Buffer newAccessToken =
        copyString(&memory->persistent, tokenJson, CS("access_token"));
    Buffer newRefreshToken =
        copyString(&memory->persistent, tokenJson, CS("refresh_token"));

    if(newAccessToken.count) {
        nst->accessToken = newAccessToken;
    }
    if(newRefreshToken.count) {
        nst->refreshToken = newRefreshToken;
    }
}

static Buffer
cStringConcat3(MemoryArena *arena,
        Buffer str1, Buffer str2, Buffer str3)
{
    Buffer newStr = {0};
    newStr.count = str1.count + str2.count + str3.count + 1;
    newStr.data = pushToMemoryArena(arena, newStr.count);
    u64 newStrCount = 0;
    for(u64 i = 0; i < str1.count; ++i) {
        u8 value = str1.data[i];
        if(value) {
            newStr.data[newStrCount++] = value;
        }
    }
    for(u64 i = 0; i < str2.count; ++i) {
        u8 value = str2.data[i];
        if(value) {
            newStr.data[newStrCount++] = value;
        }
    }
    for(u64 i = 0; i < str3.count; ++i) {
        u8 value = str3.data[i];
        if(value) {
            newStr.data[newStrCount++] = value;
        }
    }
    check(newStr.count && "newStr needs space for the null char");
    newStr.data[newStrCount] = 0;
    return newStr;
}

static void
writePlaylistIntoFile(AppMemory *memory ,Playlist const *playlist)
{
    Buffer playlistPath = 
        cStringConcat3(&memory->persistent, playlist->name,
                CS(".csv"), (Buffer){0});
    FILE *file = fopen((char*)playlistPath.data, "w");
    if(!file) {
        printWarning(
                "couldn't create playlist file \"%.*s\", "
                "skipping playlist...",
                (int)playlistPath.count, playlistPath.data);
    }
    else {
        fprintf(file,"title,album,artitsts,\"date added\",duration\n");
        for(u64 i = 0; i < playlist->trackCount; ++i) {
            Track track = playlist->trackArray[i];
            fprintf(file, "\"%.*s\",",
                    (int)track.title.count, track.title.data);
            fprintf(file, "\"%.*s\",",
                    (int)track.album.count, track.album.data);
            fprintf(file, "\"");
            for(u64 i = 0; i < track.artistCount; ++i) {
                Buffer artist = track.artistArray[i];
                fprintf(file, "%.*s", (int)artist.count, artist.data);
                b32 isLast = (i + 1 == track.artistCount);
                if(!isLast) {
                    fprintf(file, ",");
                }
            }
            fprintf(file, "\",");
            fprintf(file, "\"%.*s\",",
                    (int)track.dateAdded.count, track.dateAdded.data);

            int hours = track.durationInMs / 3600000;
            int minutes = (track.durationInMs / 60000) % 60;
            int seconds = (track.durationInMs / 1000) % 60;
            fprintf(file, "\"%02i:%02i:%02i\"\n", hours, minutes, seconds);
        }
        fclose(file);
    }
}

static void
readTracksAndCopyToFileIfDone(AppMemory *memory,
        PlaylistArray const *playlistArray, u64 playlistIndex,
        json_Element tracksJson, u64 trackOffset)
{
    Playlist *playlist = &playlistArray->data[playlistIndex];

    json_Element tracksArrayJson =
        json_getElement(tracksJson, CS("items"));
    check(tracksArrayJson.type == json_ARRAY);
    u64 trackIndex = trackOffset;
    for(json_Element *item = tracksArrayJson.firstSubElement;
            item;
            item = item->nextSibling) {

        Track track = {0};
        json_Element trackJson = json_getElement(*item, CS("track"));
        if(trackJson.type != json_OBJECT) {
            printWarning("couldn't get track's information, skipping track");
            continue;
        }

        track.title = copyStringAndEscapeCommas(
                &memory->persistent, trackJson, CS("name"));

        json_Element album = json_getElement(trackJson, CS("album"));
        track.album =
            copyStringAndEscapeCommas(&memory->persistent, album, CS("name"));

        // artists
        json_Element artistsArray = json_getElement(trackJson, CS("artists"));
        if(artistsArray.type == json_ARRAY) {
            u64 artistCount = json_getArrayCount(artistsArray);
            track.artistArray =
                pushArray(&memory->persistent, artistCount, Buffer);
            track.artistCount = artistCount;
            u64 artistIndex = 0;
            for(json_Element *artist = artistsArray.firstSubElement;
                    artist;
                    artist = artist->nextSibling) {
                check(artistIndex < artistCount);
                Buffer artistName = copyStringAndEscapeCommas(
                        &memory->persistent,*artist,CS("name"));
                track.artistArray[artistIndex++] = artistName;
            }
        }

        track.dateAdded = copyStringAndEscapeCommas(
                &memory->persistent, *item, CS("added_at"));

        json_Element durationElement =
            json_getElement(trackJson, CS("duration_ms"));
        track.durationInMs = (u64)json_getNumber(durationElement);

        playlist->trackArray[trackIndex++] = track;
        playlist->filledTrackCount += 1;
    }

    if(playlist->filledTrackCount >= playlist->trackCount) {
        check(playlist->filledTrackCount == playlist->trackCount);
        writePlaylistIntoFile(memory, playlist);
    }
}

static void
readPlaylistIdsAndQueueJobs(
        JobQueue *jq, AppMemory *memory,
        PlaylistArray const *playlistArray, u64 playlistOffset,
        json_Element playlistArrayJson) {

    u64 playlistIndex = playlistOffset;
    for(json_Element *item = playlistArrayJson.firstSubElement;
            item;
            item = item->nextSibling) {
        Buffer playlistId =
            copyString(&memory->persistent, *item, CS("id"));
        Job playlistJob = {
            .type = Job_playlistHeader,
            .uri = bufferConcat(
                    &memory->persistent, CS(PLAYLIST_URI), playlistId),
            .playlistIndex = playlistIndex,
        };
        enqueueJob(jq, playlistJob);
        playlistIndex += 1;
        check(playlistIndex <= playlistArray->count);
    }
}

static void
processJob(JobQueue *jq, AppMemory *memory,
        PlaylistArray *playlistArray, Job job)
{
    switch(job.type) {
    case Job_zero:
    {
    } break;
    case Job_playlistListHeader:
    {
        json_Element playlistListJson = job.json;
        if(!playlistListJson.type) {
            errorAndTerminate("couldn't get list of albums from spotify");
        }

        json_Element jsonTotal =
            json_getElement(playlistListJson, CS("total"));
        json_Element jsonLimit = 
            json_getElement(playlistListJson, CS("limit"));

        u64 totalPlaylistCount = json_getNumber(jsonTotal);
        u64 playlistsPerPage = json_getNumber(jsonLimit);
        u64 pageCount = playlistsPerPage ?
            (totalPlaylistCount + playlistsPerPage - 1) / playlistsPerPage : 0;

        for(u64 pageIndex = 1; pageIndex < pageCount; ++pageIndex) {
            u64 offset = playlistsPerPage * pageIndex;
            Buffer offsetString = u64ToString(&memory->persistent, offset);
            Buffer pageUri = bufferConcat5(&memory->persistent,
                    job.uri, CS("/shows?offset="), offsetString,
                    CS("&limit="), jsonLimit.value);
            Job newJob = {
                .type = Job_playlistList,
                .uri = pageUri,
                .offset = offset,
            };
            enqueueJob(jq, newJob);
        }

        playlistArray->data =
            pushArray(&memory->persistent, totalPlaylistCount, Playlist);
        playlistArray->count = totalPlaylistCount;

        check(job.offset == 0 &&
                "Job_playlistListHeader should be the first job that "
                "reads playlists ids from the user");

        json_Element playlistArrayJson =
            json_getElement(playlistListJson, CS("items"));
        if(playlistArrayJson.type != json_ARRAY) {
            errorAndTerminate("couldn't retrieve playlists from spotify");
        }
        readPlaylistIdsAndQueueJobs(jq, memory,
                playlistArray, job.offset, playlistArrayJson);
    } break;
    case Job_playlistList:
    {
        json_Element playlistListJson = job.json;
        if(!playlistListJson.type) {
            errorAndTerminate("couldn't retrieve some playlists from spotify");
        }
        json_Element playlistArrayJson =
            json_getElement(playlistListJson, CS("items"));
        if(playlistArrayJson.type != json_ARRAY) {
            errorAndTerminate("couldn't retrieve some playlists from spotify");
        }

        readPlaylistIdsAndQueueJobs(jq, memory,
                playlistArray, job.offset, playlistArrayJson);
    } break;
    case Job_playlistHeader:
    {
        json_Element playlistJson = job.json;
        if(!playlistJson.type) {
            printWarning("couldn't retrieve playlist from spotify "
                    "skipping playlist...");
        }
        else {
            Buffer playlistName =
                copyString(&memory->persistent, playlistJson, CS("name"));
            json_Element tracksJson =
                json_getElement(playlistJson, CS("tracks"));

            if(!tracksJson.type) {
                printWarning("couldn't read some tracks from playlist "
                        "\"%.*s\"\n",
                        (int)playlistName.count, playlistName.data);
            }

            fprintf(stderr, "reading playlist \"%.*s\"...\n",
                    (int)playlistName.count, playlistName.data); 

            json_Element jsonTotal =
                json_getElement(tracksJson, CS("total"));
            json_Element jsonLimit = 
                json_getElement(tracksJson, CS("limit"));

            u64 totalTracksCount = json_getNumber(jsonTotal);
            u64 tracksPerPage = json_getNumber(jsonLimit);
            u64 pageCount = tracksPerPage ?
                (totalTracksCount + tracksPerPage - 1) / tracksPerPage : 0;
            for(u64 pageIndex = 1; pageIndex < pageCount; ++pageIndex) {
                u64 offset = tracksPerPage * pageIndex;
                Buffer offsetString = u64ToString(&memory->persistent, offset);
                Buffer pageUri = bufferConcat5(&memory->persistent,
                        job.uri, CS("/tracks?offset="), offsetString,
                        CS("&limit="), jsonLimit.value);
                Job newJob = {
                    .type = Job_trackList,
                    .uri = pageUri,
                    .playlistIndex = job.playlistIndex,
                    .offset = offset,
                };
                enqueueJob(jq, newJob);
            }

            Track *trackArray =
                pushArray(&memory->persistent, totalTracksCount, Track);

            playlistArray->data[job.playlistIndex] = (Playlist) {
                .name = playlistName,
                .trackArray = trackArray,
                .trackCount = totalTracksCount,
            };

            check(job.offset == 0 &&
                    "Job_playlistHeader should be the first job that "
                    "reads tracks from a playlist");
            readTracksAndCopyToFileIfDone(
                    memory, playlistArray, job.playlistIndex,
                    tracksJson, job.offset);
        }
    } break;
    case Job_trackList:
    {
        json_Element tracksJson = job.json;
        if(!tracksJson.type) {
            printWarning("couldn't access tracks page, skipping some tracks");
        }
        readTracksAndCopyToFileIfDone(memory, playlistArray, job.playlistIndex,
                tracksJson, job.offset);

    } break;
    }
}

static void
renewAccessToken(NetworkState *nst, AppMemory *memory)
{
    CURL *handle = nst->easyHandleArray[0];
    MemoryArena *handleArena = &nst->handleToArenaMap[0];
    Buffer post = cStringConcat3(&memory->persistent,
        CS("grant_type=refresh_token&refresh_token="),
        nst->refreshToken, (Buffer){0});
    httpPostToken(handle, handleArena, post);
    long code_post = 0;

    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &code_post);
    if(code_post != OK_RESPONSE) {
        errorAndTerminate("problem while connecting with spotify");
    }
    Buffer text =
        {.data = handleArena->data, .count = handleArena->count};
    json_Element tokenJson = parseBufferToJson(&memory->scratch, text);
    json_Element error = json_getElement(tokenJson, CS("error"));
    if(error.type) {
        errorAndTerminate("problem while connecting with spotify");
    }
    getAccessTokensFromJson(nst, memory, tokenJson);
    clearMemoryArena(&memory->scratch);
}

static void
configureEasyHandleAndAddToMulti(
        NetworkState *nst, AppMemory *memory, u64 handleIndex, Job job)
{
    CURL *easyHandle = nst->easyHandleArray[handleIndex];
    MemoryArena *handleArena = &nst->handleToArenaMap[handleIndex]; 
    Buffer cStringUri = pushBufferAsCString(&memory->persistent, job.uri);
    Buffer accessTokenCString =
        pushBufferAsCString(&memory->persistent, nst->accessToken);
    curl_easy_setopt(easyHandle, CURLOPT_VERBOSE, 0);
    curl_easy_setopt(easyHandle, CURLOPT_WRITEFUNCTION,
            writeDataLibcurlCallback);
    curl_easy_setopt(easyHandle, CURLOPT_WRITEDATA, handleArena);
    curl_easy_setopt(easyHandle, CURLOPT_HTTPGET, 1);
    curl_easy_setopt(easyHandle, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
    curl_easy_setopt(easyHandle, CURLOPT_XOAUTH2_BEARER,
            accessTokenCString.data);
    curl_easy_setopt(easyHandle, CURLOPT_URL, cStringUri.data);
    CURLMcode code = curl_multi_add_handle(nst->multiHandle, easyHandle);
    check(!code);
    nst->busyHandleFlagArray[handleIndex] = 1;
    nst->busyHandleCount += 1;
    check(nst->busyHandleCount < nst->easyHandleCount);
    nst->handleToJobMap[handleIndex] = job;
}

static u64
findFreeHandle(NetworkState *nst)
{
    u64 count = nst->easyHandleCount;
    u64 freeIndex = 0;
    for(u64 i = 1; i < count; ++i) {
        if(!nst->busyHandleFlagArray[i]) {
            freeIndex = i;
            break;
        }
    }
    return freeIndex;
}

static void
removeEasyHandleFromMulti(NetworkState *nst, u64 handleIndex)
{
    CURL *easyHandle = nst->easyHandleArray[handleIndex];
    CURLMcode code = curl_multi_remove_handle(nst->multiHandle, easyHandle);
    check(!code);
    nst->busyHandleFlagArray[handleIndex] = 0;
    check(nst->busyHandleCount > 0);
    nst->busyHandleCount -= 1;
}

static void
addRequest(NetworkState *nst, AppMemory *memory, JobQueue *jq, Job job)
{
    if(!job.uri.count) {
        return;
    }
    u64 handleIndex = findFreeHandle(nst);
    if(handleIndex) {
        configureEasyHandleAndAddToMulti(nst, memory, handleIndex, job);
    }
    else {
        enqueueJob(jq, job);
    }
}

static void
updateRequests(NetworkState *nst) 
{
    int handleCount = 0;
    CURLMcode code = curl_multi_perform(nst->multiHandle, &handleCount);
    check(!code);
}

static void
waitForRequests(NetworkState *nst)
{
    int handleCount = 0;
    CURLMcode code = curl_multi_perform(nst->multiHandle, &handleCount);
    check(!code);
    if(handleCount) {
        CURLMcode code = curl_multi_poll(nst->multiHandle, 0, 0, 300, 0);
        check(!code);
    }
}

static u64
getHandleIndex(NetworkState *nst, CURL *easyHandle)
{
    u64 handleCount = nst->easyHandleCount;
    u64 handleIndex = 0;
    for(u64 i = 0; i < handleCount; ++i) {
        CURL *h = nst->easyHandleArray[i];
        if(h == easyHandle) {
            handleIndex = i;
            break;
        }
    }
    return handleIndex;
}

static void
processFinishedRequests(NetworkState *nst, JobQueue *jq,
        AppMemory *memory, PlaylistArray *playlistArray)
{
    int msgCount = 0;
    CURLMsg *msg = 0;
    b32 mustRenewAccessToken = 0;
    do {
        Job job = {0};
        msg = curl_multi_info_read(nst->multiHandle, &msgCount);
        if(msg) {
            check(msg->msg == CURLMSG_DONE &&
                    "that should be the only defined type in libcurl");
            CURL *easyHandle =
                (msg->msg == CURLMSG_DONE) ? msg->easy_handle : 0;
            if(easyHandle) {
                u64 handleIndex = getHandleIndex(nst, easyHandle);
                MemoryArena *handleArena = &nst->handleToArenaMap[handleIndex];
                job = nst->handleToJobMap[handleIndex];
                long responseCode = 0;
                CURLcode c = curl_easy_getinfo(
                        easyHandle, CURLINFO_RESPONSE_CODE, &responseCode);
                check(!c);
                if(responseCode == EXPIRED_TOKEN_RESPONSE) {
                    mustRenewAccessToken = 1;
                    enqueueJob(jq, job);
                    job = (Job){0};
                }
                else if(responseCode != OK_RESPONSE) {
                    errorAndTerminate("problem while connecting with spotify");
                }
                Buffer text = {
                    .data = handleArena->data,
                    .count = handleArena->count
                };
                job.json = parseBufferToJson(&memory->scratch, text);
                clearMemoryArena(handleArena);
                removeEasyHandleFromMulti(nst, handleIndex);
            }
        }
        processJob(jq, memory, playlistArray, job);
        clearMemoryArena(&memory->scratch);
    } while(msg);

    if(mustRenewAccessToken) {
        renewAccessToken(nst, memory);
    }
}

static void
initNetworkState(NetworkState *nst, MemoryArena *arena)
{
    nst->multiHandle = curl_multi_init();
    check(nst->multiHandle);

    u64 easyCount = CONNECTION_COUNT + 1;
    nst->easyHandleCount = easyCount;
    nst->easyHandleArray = pushArray(arena, easyCount, CURL*);
    for(u64 i = 0; i < easyCount; ++i) {
        CURL *easy = curl_easy_init();
        check(easy);
        nst->easyHandleArray[i] = easy;
    }
    
    nst->handleToJobMap      = pushArray(arena, easyCount, Job);
    nst->busyHandleFlagArray = pushArray(arena, easyCount, b32);
    nst->handleToArenaMap    = pushArray(arena, easyCount, MemoryArena);
    for(u64 i = 0; i < easyCount; ++i) {
        MemoryArena easyHandleArena = allocateMemoryArena(5*MEGABYTE);
        nst->handleToArenaMap[i] = easyHandleArena;
    }
}

static b32
areAllHandlesBusy(NetworkState const *nst)
{
    // the +1 is for the handle at index 0, which is reserved for special kinds
    // of requests
    check(nst->busyHandleCount + 1 <= nst->easyHandleCount);
    return nst->busyHandleCount + 1 == nst->easyHandleCount;
}

int
main(int argc, char **argv)
{
    State state = {0};
    State *st = &state;

    // init state
    {
        initLibcurl(st);

        st->memory.curlBuffer = allocateMemoryArena(5*MEGABYTE);
        st->memory.persistent = allocateMemoryArena(5*MEGABYTE);
        st->memory.scratch    = allocateMemoryArena(5*MEGABYTE);

        initNetworkState(&st->networkState, &st->memory.persistent);
        initJobQueue(&st->jobQueue, 1024, &st->memory.persistent);
    }

    if(argc != 2) {
        errorAndTerminate("wrong parameters\nusage: %s AUTHORIZATION_CODE\n"
                "link for authorization code:\n"
                AUTHORIZATION_CODE_ACCESS_URI,
                argv[0]);
    }

    NetworkState *nst = &st->networkState;

    // construct and send POST request
    {
        Buffer authorizationCode =
                {.data = (u8*)(argv[1]), strlen(argv[1]) + 1};
        Buffer post = cStringConcat3(&st->memory.persistent,
                CS("grant_type=authorization_code&code="),
                authorizationCode,
                CS("&redirect_uri="REDIRECT_URI)); // string isn't copied to libcurl, so we must keep it in memory
        CURL *handle = nst->easyHandleArray[0];
        MemoryArena *handleArena = &nst->handleToArenaMap[0];
        httpPostToken(handle, handleArena, post);

        // get access tokens from json
        {
            Buffer text =
                {.data = handleArena->data, .count = handleArena->count};
            json_Element tokensJson =
                parseBufferToJson(&st->memory.scratch, text);
            json_Element error = json_getElement(tokensJson, CS("error"));
            if(error.type) {
                errorAndTerminate("invalid authorization code, "
                        "check if you copied it correctly");
            }
            getAccessTokensFromJson(nst, &st->memory, tokensJson);
            clearMemoryArena(&st->memory.scratch);
        }
    }


    JobQueue *jq = &st->jobQueue;
    {
        Job job = {
            .type = Job_playlistListHeader,
            .uri = CS(PLAYLIST_LIST_URI),
        };
        enqueueJob(jq, job);
    }

    while(!isJobQueueEmpty(jq) || nst->busyHandleCount) {
        while(!isJobQueueEmpty(jq) && !areAllHandlesBusy(nst)) {
            Job job = dequeueJob(jq);
            addRequest(nst, &st->memory, jq, job);
        }
        updateRequests(nst);
        processFinishedRequests(nst, jq, &st->memory, &st->playlistArray);

        if(isJobQueueEmpty(jq)) {
            waitForRequests(nst);
        }
    }

    deinit(st);

    return 0;
}
