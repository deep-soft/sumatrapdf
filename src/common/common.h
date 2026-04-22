#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <ole2.h>
#include <tlhelp32.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>
#include <new>     // for placement new
#include <utility> // for std::forward

typedef unsigned __int64 u64;
typedef LONG AtomicBool;
typedef LONG AtomicInt;

// Atomic bool operations (base.cpp)
bool AtomicBoolGet(AtomicBool* p);
void AtomicBoolSet(AtomicBool* p, bool v);

// Atomic int operations (base.cpp)
int AtomicIntGet(AtomicInt* p);
void AtomicIntSet(AtomicInt* p, int v);
int AtomicIntAdd(AtomicInt* p, int v);
int AtomicIntInc(AtomicInt* p);
int AtomicIntDec(AtomicInt* p);

// allocator.cpp
struct ArenaPage;

class IAllocator {
  public:
    virtual ~IAllocator();
    virtual void* Alloc(int size) = 0;
    virtual void Free(void* ptr) = 0;

    // For optimized formatting - default implementations do nothing
    virtual void Lock() {}
    virtual void Unlock() {}
    virtual void* GetAvailableSpace(int* bufSizeOut) {
        *bufSizeOut = 0;
        return nullptr;
    }
};

void* Alloc(IAllocator* a, int size);
void Free(IAllocator* a, void* mem);

void* ReallocMem(IAllocator* a, void* els, int* cap, int newCap, int elSize);
void* ReallocToWantedSize(IAllocator* a, void* els, int* cap, int wantedSize, int elSize);

// Allocate and construct object using placement new (supports constructor args)
template <typename T, typename... Args>
T* New(IAllocator* a, Args&&... args) {
    void* mem = a->Alloc((int)sizeof(T));
    return new (mem) T(std::forward<Args>(args)...);
}


// Arena allocator - allocates from pages, Free() does nothing
class ArenaAllocator : public IAllocator {
  public:
    ArenaAllocator();
    ~ArenaAllocator();

    void* Alloc(int size) override;
    void Free(void* ptr) override;

    void Reset();        // Free all pages except the first
    void FreeAllPages(); // Free all pages

    // Lock/Unlock for multi-step operations (e.g., GetAvailableSpace + Alloc)
    void Lock() override;
    void Unlock() override;

    // Get pointer to available space in current page and its size
    // MUST be called while locked (asserts if not)
    void* GetAvailableSpace(int* bufSizeOut) override;

    // Statistics (atomic for thread safety)
    AtomicInt totalAllocations;       // Total allocations ever
    AtomicInt totalAllocatedBytes;    // Total bytes ever allocated
    AtomicInt currentAllocations;     // Allocations since last Reset()
    AtomicInt currentAllocatedBytes;  // Bytes allocated since last Reset()
    AtomicInt maxAllocationsPerReset; // Max allocations between Resets
    AtomicInt maxBytesPerReset;       // Max bytes between Resets

  private:
    ArenaPage* AllocPage(int minDataSize);

    CRITICAL_SECTION cs;
    ArenaPage* firstPage;
    ArenaPage* currentPage;
    int nextPageSize;
    bool isLocked; // For debug assertion
};

// Thread-local temporary allocator, reset after each message loop iteration
extern thread_local ArenaAllocator* gTempAllocator;
ArenaAllocator* GetTempAllocator();

// Standalone reserve/commit arena
static const u64 ARENA_HEADER_SIZE = 128;

typedef u64 ArenaFlags;
enum : ArenaFlags {
    ArenaFlag_NoChain = 1ull << 0,
    ArenaFlag_LargePages = 1ull << 1,
};

struct ArenaParams {
    ArenaFlags flags = 0;
    u64 reserve_size = 0;
    u64 commit_size = 0;
    void* optional_backing_buffer = nullptr;
    const char* allocation_site_file = nullptr;
    int allocation_site_line = 0;
    const char* name = nullptr;
};

struct Arena {
    Arena* prev;    // Previous arena in chain
    Arena* current; // Current arena in chain
    ArenaFlags flags;
    u64 cmt_size;
    u64 res_size;
    u64 base_pos;
    u64 pos;
    u64 cmt;
    u64 res;
    const char* allocation_site_file;
    int allocation_site_line;
    const char* name;
    bool uses_external_buffer;
};
static_assert(sizeof(Arena) <= ARENA_HEADER_SIZE, "Arena header must fit in reserved header bytes");

struct Temp {
    Arena* arena;
    u64 pos;
};

extern u64 arena_default_reserve_size;
extern u64 arena_default_commit_size;
extern ArenaFlags arena_default_flags;

ArenaParams ArenaDefaultParams();
Arena* arena_alloc(const ArenaParams& params = ArenaDefaultParams());
void arena_release(Arena* arena);
void* arena_push(Arena* arena, u64 size, u64 align = 8, bool zero = true);
u64 arena_pos(Arena* arena);
void arena_pop_to(Arena* arena, u64 pos);
void arena_clear(Arena* arena);
void arena_pop(Arena* arena, u64 amt);
Temp temp_begin(Arena* arena);
void temp_end(Temp temp);

template <typename T>
inline T* push_array_no_zero_aligned(Arena* arena, u64 count, u64 align) {
    return (T*)arena_push(arena, sizeof(T) * count, align, false);
}

template <typename T>
inline T* push_array_aligned(Arena* arena, u64 count, u64 align) {
    return (T*)arena_push(arena, sizeof(T) * count, align, true);
}

template <typename T>
inline T* push_array_no_zero(Arena* arena, u64 count) {
    return push_array_no_zero_aligned<T>(arena, count, (alignof(T) > 8) ? alignof(T) : 8);
}

template <typename T>
inline T* push_array(Arena* arena, u64 count) {
    return push_array_aligned<T>(arena, count, (alignof(T) > 8) ? alignof(T) : 8);
}
// works on any struct with len member (Str, WStr, *Vec)
template <typename T>
int len(const T& v) {
    return v.len;
}
template <typename T>
int len(const T* v) {
    return v->len;
}

template<typename T>
void VecExpandTo(IAllocator* a, T& v, int wantedSize) {
    if (wantedSize <= v.cap) {
        return;
    }
    v.els = (decltype(v.els))ReallocToWantedSize(a, v.els, &v.cap, wantedSize, (int)sizeof(*v.els));
}

template <typename T>
void VecExpand(IAllocator* a, T& v, int n) {
    int wantedSize = v.len + n;
    VecExpandTo(a, v, wantedSize);
}

template <typename T, typename E>
void VecPush(IAllocator* a, T& v, E el) {
    VecExpand(a, v, 1);
    v.els[v.len] = el;
    v.len++;
}

// Iterator wrapper for range-based for loops over Vec types (structs with len/els)
template <typename Vec>
class VecIterator {
    Vec* vec;

  public:
    VecIterator(Vec* v) : vec(v) {}
    auto begin() { return vec ? vec->els : nullptr; }
    auto end() { return vec ? vec->els + vec->len : nullptr; }
};

// Helper functions for type deduction (works with both Vec& and Vec*)
template <typename Vec>
VecIterator<Vec> VecIter(Vec& v) {
    return VecIterator<Vec>(&v);
}
template <typename Vec>
VecIterator<Vec> VecIter(Vec* v) {
    return VecIterator<Vec>(v);
}

// str_util.cpp

struct Str {
    char* s;
    int len;

    Str() : s(nullptr), len(0) {}
    explicit Str(char* s_) : s(s_), len(0) {
        while (s && s[len]) len++;
    }
    explicit Str(char* s_, int len_) : s(s_), len(len_) {}
};

// Create Str from string literal with compile-time length
#define StrL(lit) Str((char*)(lit), (int)(sizeof(lit) - 1))

struct StrVec {
    int len;
    int cap;
    Str* els;
};

void SplitStrByWhitespace(IAllocator* a, const Str& s, StrVec& vecOut);

struct WStr {
    wchar_t* s;
    int len;

    WStr() : s(nullptr), len(0) {}
    explicit WStr(wchar_t* s_) : s(s_), len(0) {
        while (s && s[len]) len++;
    }
    explicit WStr(wchar_t* s_, int len_) : s(s_), len(len_) {}
};

// Create WStr from wide string literal with compile-time length
#define WStrL(lit) WStr((wchar_t*)(lit), (int)(sizeof(lit) / sizeof(wchar_t) - 1))

bool WStrEq(WStr a, WStr b);
void WStrCopy(wchar_t* dst, const wchar_t* src, int maxLen);
wchar_t ToLowerW(wchar_t c);
int WStrFindSubstr(WStr str, WStr substr);
int WStrCmpNoCase(WStr a, WStr b);

WStr ToWStrTemp(const char* utf8);
WStr ToWStrTemp(Str s);
Str ToUtf8(IAllocator* a, WStr wide);
Str ToUtf8Temp(WStr wide);

// Str utilities
Str StrDup(IAllocator* a, Str s);
bool StrEq(Str a, Str b);
bool StrContains(Str str, Str substr);
bool StrHasPrefix(Str s, Str prefix);
bool StrHasSuffix(Str s, Str suffix);
Str StrTrimSuffix(Str s, Str suffix);
bool StrHasPrefixNoCase(Str s, Str prefix);
Str FormatFileSize(IAllocator* a, u64 size);
void FormatFileSizeToWstrBuf(u64 size, WStr buf);
int FormatSizeHumanIntoBuf(u64 size, Str buf);
void FormatSizeHumanIntoWBuf(u64 size, WStr wbuf);
Str PathJoinTemp(Str dir, Str name);
Str StrFmt(IAllocator* a, const char* fmt, ...);
#define StrFmtTemp(fmt, ...) StrFmt(GetTempAllocator(), fmt, __VA_ARGS__)
Str StrDupTemp(Str s);
int StrLastIndexOfChar(Str s, char c);

// Counters for StrFmt optimization tracking
extern AtomicInt gStrFmtFirstAlloc;  // Formatted into available space
extern AtomicInt gStrFmtSecondAlloc; // Needed separate allocation

// file_util.cpp
bool FileSystemEntryExists(Str s);
bool DirectoryExists(Str s);
bool FileExists(Str s);
Str FindFirstValidParentDir(Str path);
Str PathGetDirTemp(Str path);
Str PathGetNameTemp(Str path);
Str SmartResolveDirectory(Str dir);

// UTF-8 string utilities (legacy, for null-terminated strings)
void StrCopyUtf8(char* dst, const char* src, int maxBytes);
Str StrTrimSuffixWhitespace(Str s);

// Works for any struct with int len member (Str, WStr, *Vec, etc.)
template <typename T>
inline bool IsEmpty(const T& v) {
    return v.len <= 0;
}
template <typename T>
inline bool IsEmpty(const T* v) {
    return !v || v->len <= 0;
}

// specialized for Str and WStr
inline bool IsEmpty(const Str& v) {
    return !v.s || v.len <= 0;
}
inline bool IsEmpty(const Str* v) {
    return !v || !v->s || v->len <= 0;
}
inline bool IsEmpty(const WStr& v) {
    return !v.s || v.len <= 0;
}
inline bool IsEmpty(const WStr* v) {
    return !v || !v->s || v->len <= 0;
}

// dir_util.cpp

// Forward declaration for DirEntry
struct DirEntries;

// Sentinel value indicating directory is still being scanned
#define kStillScanningDir ((DirEntries*)(uintptr_t)-2)

// Check if DirEntry is a directory (dv != nullptr means it's a dir)
inline bool IsDir(DirEntries* dv) {
    return dv != nullptr;
}

struct DirEntry {
    Str name;
    u64 size;
    DirEntries* dv; // nullptr=file, kStillScanningDir=dir not yet scanned, else=scanned dir
    FILETIME createTime;
    FILETIME modTime;
};

struct DirEntries {
    Str fullDir; // Full path of this directory
    int len;
    DirEntry* els;
    char* err; // Error message if directory couldn't be read, nullptr otherwise
};

struct DirEntriesNode {
    DirEntriesNode* next;
    DirEntries* dv;
    bool nonRecursive; // If true, don't queue subdirectories for scanning
};

// Callback type for when a directory scan completes
typedef void (*OnScannedDirCallback)(DirEntries* dv, void* userData);

// Background directory reader thread context
struct DirScanCtx {
    IAllocator* a; // Thread-safe arena allocator for permanent data
    OnScannedDirCallback onScannedDir;
    void* userData;
    CRITICAL_SECTION cs;         // Protect queue access
    HANDLE hSemaphore;           // Counting semaphore for work items
    HANDLE hQueueEmptyEvent;     // Signaled when all work is done (queue empty + no in-flight)
    HANDLE hThreadExitedEvent;   // Signaled when thread has exited
    DirEntriesNode* dirsToVisit; // Queue of directories to scan
    AtomicBool shouldExit;       // Signal thread to exit
    AtomicInt inFlightCount;     // Number of directories currently being processed
};

DirScanCtx* CreateDirScanCtx(IAllocator* a, OnScannedDirCallback callback, void* userData);
void AskDirScanThreadToQuit(DirScanCtx* ctx);
DirEntries* RequestDirScan(DirScanCtx* ctx, Str dir);
void QueueDirScan(DirScanCtx* ctx, DirEntries* dv, bool nonRecursive = false);
void RequestDirRescan(DirScanCtx* ctx, DirEntries* dv);

// Directory utilities (paths are UTF-8)
DirEntry* FindEntryByName(DirEntries* dv, Str name);
DWORD WINAPI DirScanThread(LPVOID param);

// Allocate a DirEntries with fullDir set
DirEntries* AllocDirEntries(IAllocator* a, Str fullDir);
void ReadDirectory(IAllocator* a, DirEntries* dv, LONG* shouldExit);

// win_util.cpp
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// DC state management
struct SavedDCState {
    HWND hwnd;
    HDC hdc;
    HFONT oldFont;
};

SavedDCState SaveDCState(HWND hwnd);
void RestoreDCState(SavedDCState* state);
int MeasureStringWidth(HDC hdc, const wchar_t* str);
Str GetWindowTextTemp(HWND hwnd);
void SetHwndText(HWND hwnd, Str s);
Str GetLastErrorAsStr(IAllocator* a);
bool WasLaunchedByPowershellWithPipeRedirect();
Str GetAppLocalDataDirTemp();

// log.cpp
void LogInit(Str logFilePath);
void LogDestroy();
void logStr(Str s);
#define logf(fmt, ...) logStr(StrFmtTemp(fmt, __VA_ARGS__))
void logConsole(const char* fmt, ...);
void WaitForConsoleClose();
void SendEnterIfLoggedToConsole();

