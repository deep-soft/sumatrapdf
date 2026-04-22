#include "common.h"
#include <assert.h>

void* Alloc(IAllocator* a, int size) {
    if (!a) {
        return malloc(size);
    }
    return a->Alloc(size);
}

void Free(IAllocator* a, void* mem) {
    if (!a) {
        free(mem);
    } else {
        a->Free(mem);
    }
}

// Thread-local temporary allocator, reset after each message loop iteration
thread_local ArenaAllocator* gTempAllocator = nullptr;

ArenaAllocator* GetTempAllocator() {
    if (!gTempAllocator) {
        gTempAllocator = new ArenaAllocator();
    }
    return gTempAllocator;
}

// Page structure for ArenaAllocator
struct ArenaPage {
    ArenaPage* prev;
    ArenaPage* next;
    int size;       // Total size of page (including header)
    int used;       // Bytes used (not including header)
    int allocCount; // Number of allocations in this page
    // Data follows immediately after this struct
};

static const int kPageHeaderSize = sizeof(ArenaPage);
static const int kMinPageSize = 4 * 1024;
static const int kMaxPageSize = 64 * 1024;

// IAllocator base class
IAllocator::~IAllocator() {}

// ArenaAllocator implementation
ArenaAllocator::ArenaAllocator() {
    InitializeCriticalSection(&cs);
    firstPage = nullptr;
    currentPage = nullptr;
    nextPageSize = kMinPageSize;
    isLocked = false;

    // Initialize statistics
    totalAllocations = 0;
    totalAllocatedBytes = 0;
    currentAllocations = 0;
    currentAllocatedBytes = 0;
    maxAllocationsPerReset = 0;
    maxBytesPerReset = 0;
}

ArenaAllocator::~ArenaAllocator() {
    FreeAllPages();
    DeleteCriticalSection(&cs);
}

void ArenaAllocator::FreeAllPages() {
    ArenaPage* page = firstPage;
    while (page) {
        ArenaPage* next = page->next;
        free(page);
        page = next;
    }
    firstPage = nullptr;
    currentPage = nullptr;
    nextPageSize = kMinPageSize;
}

ArenaPage* ArenaAllocator::AllocPage(int minDataSize) {
    int pageSize = nextPageSize;

    // If requested size is bigger than page size, allocate exact size
    int requiredSize = minDataSize + kPageHeaderSize;
    if (requiredSize > pageSize) {
        pageSize = requiredSize;
    }

    ArenaPage* page = (ArenaPage*)malloc(pageSize);
    if (!page) return nullptr;

    page->prev = nullptr;
    page->next = nullptr;
    page->size = pageSize;
    page->used = 0;
    page->allocCount = 0;

    // Double page size for next allocation, up to max
    if (nextPageSize < kMaxPageSize) {
        nextPageSize *= 2;
        if (nextPageSize > kMaxPageSize) {
            nextPageSize = kMaxPageSize;
        }
    }

    return page;
}

void* ArenaAllocator::Alloc(int size) {
    if (size <= 0) return nullptr;

    EnterCriticalSection(&cs);

    // Align to 8 bytes
    int alignedSize = (size + 7) & ~7;

    // Check if current page has space
    if (currentPage) {
        int available = currentPage->size - kPageHeaderSize - currentPage->used;
        if (alignedSize <= available) {
            void* ptr = (char*)currentPage + kPageHeaderSize + currentPage->used;
            currentPage->used += alignedSize;
            currentPage->allocCount++;
            LeaveCriticalSection(&cs);

            // Update statistics (atomic)
            AtomicIntInc(&totalAllocations);
            AtomicIntAdd(&totalAllocatedBytes, alignedSize);
            AtomicIntInc(&currentAllocations);
            AtomicIntAdd(&currentAllocatedBytes, alignedSize);
            return ptr;
        }
    }

    // Need a new page
    ArenaPage* newPage = AllocPage(alignedSize);
    if (!newPage) {
        LeaveCriticalSection(&cs);
        return nullptr;
    }

    // Link into list
    if (currentPage) {
        currentPage->next = newPage;
        newPage->prev = currentPage;
    } else {
        firstPage = newPage;
    }
    currentPage = newPage;

    // Allocate from new page
    void* ptr = (char*)newPage + kPageHeaderSize;
    newPage->used = alignedSize;
    newPage->allocCount = 1;

    LeaveCriticalSection(&cs);

    // Update statistics (atomic)
    AtomicIntInc(&totalAllocations);
    AtomicIntAdd(&totalAllocatedBytes, alignedSize);
    AtomicIntInc(&currentAllocations);
    AtomicIntAdd(&currentAllocatedBytes, alignedSize);
    return ptr;
}

void ArenaAllocator::Free(void* ptr) {
    // Arena allocator doesn't free individual allocations
    (void)ptr;
}

void ArenaAllocator::Lock() {
    EnterCriticalSection(&cs);
    isLocked = true;
}

void ArenaAllocator::Unlock() {
    isLocked = false;
    LeaveCriticalSection(&cs);
}

void* ArenaAllocator::GetAvailableSpace(int* bufSizeOut) {
    // Must be called while locked
    assert(isLocked);

    if (!currentPage) {
        *bufSizeOut = 0;
        return nullptr;
    }

    int available = currentPage->size - kPageHeaderSize - currentPage->used;
    void* ptr = (char*)currentPage + kPageHeaderSize + currentPage->used;

    *bufSizeOut = available;
    return ptr;
}

void ArenaAllocator::Reset() {
    EnterCriticalSection(&cs);

    // Update max statistics before resetting current values
    LONG curAllocs = AtomicIntGet(&currentAllocations);
    LONG curBytes = AtomicIntGet(&currentAllocatedBytes);
    if (curAllocs > AtomicIntGet(&maxAllocationsPerReset)) {
        AtomicIntSet(&maxAllocationsPerReset, curAllocs);
    }
    if (curBytes > AtomicIntGet(&maxBytesPerReset)) {
        AtomicIntSet(&maxBytesPerReset, curBytes);
    }

    // Reset current statistics
    AtomicIntSet(&currentAllocations, 0);
    AtomicIntSet(&currentAllocatedBytes, 0);

    if (!firstPage) {
        LeaveCriticalSection(&cs);
        return;
    }

    // Free all pages except the first
    ArenaPage* page = firstPage->next;
    while (page) {
        ArenaPage* next = page->next;
        free(page);
        page = next;
    }

    // Reset first page
    firstPage->next = nullptr;
    firstPage->used = 0;
    firstPage->allocCount = 0;

    currentPage = firstPage;
    nextPageSize = kMinPageSize;

    LeaveCriticalSection(&cs);
}

void* ReallocMem(IAllocator* a, void* els, int* cap, int newCap, int elSize) {
    if (!a || newCap <= 0 || elSize <= 0) return els;

    int newSize = newCap * elSize;
    void* newEls = a->Alloc(newSize);
    if (!newEls) return els;

    // Copy old data if exists
    if (els && *cap > 0) {
        int oldSize = *cap * elSize;
        memcpy(newEls, els, oldSize);
    }

    *cap = newCap;
    return newEls;
}

// TODO: ensure not inlined
void* ReallocToWantedSize(IAllocator* a, void* els, int* cap, int wantedSize, int elSize) {
    int newCap = (*cap == 0) ? 8 : *cap * 2;
    while (newCap < wantedSize) {
        newCap *= 2;
    }
    return ReallocMem(a, els, cap, newCap, elSize);
}
