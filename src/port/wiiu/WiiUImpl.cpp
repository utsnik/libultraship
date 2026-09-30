#ifdef __WIIU__
#ifndef WIIU_DIAGNOSTICS
#define WIIU_DIAGNOSTICS 1
#endif

#include <malloc.h>
#include "port/wiiu/WiiUImpl.h"

#include <exception>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <typeinfo>
#include <unistd.h>
#include <sys/iosupport.h>

#include <coreinit/debug.h>
#if WIIU_DIAGNOSTICS
#include <whb/log.h>
#include <whb/log_udp.h>
#endif

#include <ship/window/Window.h>
#include <ship/Context.h>
#include <ship/resource/archive/O2rArchive.h>

#include "port/wiiu/WiiUWatchdog.h"

#if WIIU_DIAGNOSTICS
#include <coreinit/thread.h>
#include <coreinit/alarm.h>
#include <coreinit/dynload.h>
#include <coreinit/time.h>
#endif
#include <coreinit/exception.h>
#include <coreinit/memheap.h>
#include <coreinit/memexpheap.h>
#if WIIU_DIAGNOSTICS
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <coreinit/core.h>
#include <sysapp/launch.h>
#include <nsysnet/_socket.h>
#endif
#include <cstdlib>
#include <atomic>
#include <errno.h>
#include <new>
#include <stdint.h>

namespace Ship {
namespace WiiU {
namespace Watchdog {
#if WIIU_DIAGNOSTICS
void Emit(const char* fmt, ...);
#endif
}
}
}

extern "C" {
void* __real_malloc(size_t);
void __real_free(void*);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t);
void* __real_aligned_alloc(size_t, size_t);
// Strong, not weak: a weak reference does not pull libstdc++'s new_op.o out of the archive, so
// __real__Znwj resolved to 0 and every operator new threw bad_alloc before main() ("Abort called").
// Only the 32-bit (unsigned int) forms exist on this target; _Znwm/_Znam are not wrapped.
void* __real__Znwj(size_t);
void* __real__Znaj(size_t);
}

// This tracker is intentionally independent of the watchdog's C++/socket path.  The wrapped
// allocators may run before Watchdog::Start(), while another thread is stopped in the allocator,
// so every data structure here is static and every operation is bounded and allocation-free.
namespace {

static uint32_t BigHeapFreeBytes();
static uint32_t BigHeapLargestFree();

#if WIIU_DIAGNOSTICS
// 262144 x 32 bytes = 8 MB. The first 16384-entry table would fill with long-lived allocations
// and then drop exactly the new ones a leak is made of.
static const uint32_t kLeakTableSize = 262144;
// Deleted slots never become empty again, so an uncapped linear probe degrades to a whole-table
// scan under the lock once churn has touched every slot. Insert and lookup share this cap, so an
// entry that was inserted is always found.
static const uint32_t kLeakMaxProbe = 64;
static const uint32_t kLeakSiteTableSize = 16384;
// The allocator wrappers always stay (they route large blocks to their own heap); this switches
// only the per-call-site tracking. On, boot takes ~40 s and HOME can hang the console.
#ifndef WIIU_LEAK_TRACKER
#define WIIU_LEAK_TRACKER 0
#endif
static const uint32_t kLeakTopCount = 12;
static const uint32_t kLeakChurnTopCount = 10;
static const uint64_t kLeakBigThreshold = 64u * 1024u;
static const uint32_t kLeakEmpty = 0;
static const uint32_t kLeakUsed = 1;
static const uint32_t kLeakDeleted = 2;

struct LeakEntry {
    uintptr_t address;
    uint64_t bytes;
    uint32_t key[3];
    int32_t site;
    uint32_t state;
};

struct LeakSite {
    uint32_t key[3];
    uint64_t liveBytes;
    uint32_t liveCount;
    uint64_t bigLiveBytes;
    uint32_t bigLiveCount;
    uint64_t previousBytes;
    uint64_t churnBytes;
    uint32_t churnCount;
    uint64_t churnMin;
    uint64_t churnMax;
    uint32_t used;
};

struct LeakTop {
    uint32_t key[3];
    uint64_t bytes;
    uint32_t count;
    uint64_t growth;
};

struct LeakChurnTop {
    uint32_t key[3];
    uint64_t bytes;
    uint32_t count;
    uint64_t min;
    uint64_t max;
};

// Allocated on first use, not static: as .bss these were 11.5 MB and soh923m1 never reached main().
static LeakEntry* sLeakTable = nullptr;
static LeakSite* sLeakSites = nullptr;
static volatile uint32_t sLeakTablesState = 0; // 0 = not tried, 1 = initialising, 2 = ready, 3 = failed
static volatile uint32_t sLeakLock = 0;
static volatile uint32_t sLeakTracked = 0;
static volatile uint32_t sLeakDropped = 0;
// CafeOS RPLs reject the TLS relocations emitted for __thread.  This is the same semantic
// guard, backed by a fixed slot table keyed by the current OSThread, so it remains per-thread
// without using TLS or allocating to discover a thread's state.
struct LeakThreadState {
    OSThread* thread;
    volatile uint32_t reentrant;
    volatile uint32_t newDepth;
};
static LeakThreadState sLeakThreadStates[128];
static LeakThreadState sLeakFallbackState = { nullptr, 0, 0 };

static LeakThreadState* LeakCurrentThreadState() {
    OSThread* current = OSGetCurrentThread();
    if (current == nullptr) {
        return &sLeakFallbackState;
    }
    for (uint32_t index = 0; index < sizeof(sLeakThreadStates) / sizeof(sLeakThreadStates[0]); ++index) {
        LeakThreadState& state = sLeakThreadStates[index];
        if (state.thread == current) {
            return &state;
        }
        if (state.thread == nullptr &&
            __sync_bool_compare_and_swap(&state.thread, (OSThread*)nullptr, current)) {
            return &state;
        }
    }
    return &sLeakFallbackState;
}

extern "C" {
void* __wrap_malloc(size_t);
void __wrap_free(void*);
void* __wrap_calloc(size_t, size_t);
void* __wrap_realloc(void*, size_t);
void* __wrap_memalign(size_t, size_t);
int __wrap_posix_memalign(void**, size_t, size_t);
void* __wrap_aligned_alloc(size_t, size_t);
void* __wrap__Znwj(size_t);
void* __wrap__Znaj(size_t);
}

static inline uint32_t LeakHashWord(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    return value ^ (value >> 16);
}

static uint32_t LeakKeyHash(const uint32_t key[3]) {
    uint32_t hash = 2166136261u;
    hash = (hash ^ LeakHashWord(key[0])) * 16777619u;
    hash = (hash ^ LeakHashWord(key[1])) * 16777619u;
    hash = (hash ^ LeakHashWord(key[2])) * 16777619u;
    return hash;
}

static uint32_t LeakAddressHash(uintptr_t address) {
    return LeakHashWord((uint32_t)address ^ ((uint32_t)address >> 16));
}

static bool LeakIsWrapperFrame(uint32_t address) {
    // The normal back-chain walk starts below the wrapper frame, but retain this filter for
    // toolchain variants which expose the wrapper's saved LR as one of the walk results.
    return address == (uint32_t)(uintptr_t)&__wrap_malloc ||
           address == (uint32_t)(uintptr_t)&__wrap_free ||
           address == (uint32_t)(uintptr_t)&__wrap_calloc ||
           address == (uint32_t)(uintptr_t)&__wrap_realloc ||
           address == (uint32_t)(uintptr_t)&__wrap_memalign ||
           address == (uint32_t)(uintptr_t)&__wrap_posix_memalign ||
           address == (uint32_t)(uintptr_t)&__wrap_aligned_alloc ||
           address == (uint32_t)(uintptr_t)&__wrap__Znwj ||
           address == (uint32_t)(uintptr_t)&__wrap__Znaj;
}

static void LeakCaptureKey(uint32_t key[3], bool skipOperatorNew) __attribute__((noinline));
static void LeakCaptureKey(uint32_t key[3], bool skipOperatorNew) {
    key[0] = 0;
    key[1] = 0;
    key[2] = 0;
    if (!WIIU_LEAK_TRACKER) {
        return;
    }

    uint32_t* frame;
    __asm__ volatile("mr %0, 1" : "=r"(frame));
    bool skippedNew = !skipOperatorNew;
    uint32_t count = 0;
    for (uint32_t depth = 0; depth < 32 && frame != nullptr; ++depth) {
        const uintptr_t frameAddress = (uintptr_t)frame;
        if ((frameAddress & 3u) != 0 || frameAddress < 0x10000000u || frameAddress >= 0x50000000u) {
            break;
        }
        uint32_t* next = (uint32_t*)frame[0];
        const uintptr_t nextAddress = (uintptr_t)next;
        if (next == nullptr || next <= frame || (nextAddress & 3u) != 0 ||
            nextAddress < 0x10000000u || nextAddress > 0x4FFFFFFCu) {
            break;
        }

        const uint32_t returnAddress = next[1];
        frame = next;
        if (returnAddress == 0 || LeakIsWrapperFrame(returnAddress)) {
            continue;
        }
        if (!skippedNew) {
            skippedNew = true;
            continue;
        }
        if (count < 3) {
            key[count++] = returnAddress;
        }
        if (count == 3) {
            break;
        }
    }
}

static void LeakLock() {
    // OSYieldThread never runs a LOWER-priority thread, so a high-priority thread spinning here
    // on the holder's core would wait forever. Sleep instead after a short spin.
    uint32_t spins = 0;
    while (!__sync_bool_compare_and_swap(&sLeakLock, 0, 1)) {
        if (++spins < 64) {
            OSYieldThread();
        } else {
            OSSleepTicks(OSMicrosecondsToTicks(50));
        }
    }
}

static bool LeakTryLock() {
    return __sync_bool_compare_and_swap(&sLeakLock, 0, 1);
}

static void LeakUnlock() {
    __sync_synchronize();
    sLeakLock = 0;
}

static int32_t LeakFindSiteLocked(const uint32_t key[3], bool create) {
    const uint32_t start = LeakKeyHash(key) & (kLeakSiteTableSize - 1);
    for (uint32_t probe = 0; probe < kLeakSiteTableSize; ++probe) {
        LeakSite& site = sLeakSites[(start + probe) & (kLeakSiteTableSize - 1)];
        if (!site.used) {
            if (!create) {
                return -1;
            }
            site.key[0] = key[0];
            site.key[1] = key[1];
            site.key[2] = key[2];
            site.liveBytes = 0;
            site.liveCount = 0;
            site.bigLiveBytes = 0;
            site.bigLiveCount = 0;
            site.previousBytes = 0;
            site.churnBytes = 0;
            site.churnCount = 0;
            site.churnMin = 0;
            site.churnMax = 0;
            site.used = 1;
            return (int32_t)((start + probe) & (kLeakSiteTableSize - 1));
        }
        if (site.key[0] == key[0] && site.key[1] == key[1] && site.key[2] == key[2]) {
            return (int32_t)((start + probe) & (kLeakSiteTableSize - 1));
        }
    }
    return -1;
}

static int32_t LeakFindEntryLocked(uintptr_t address) {
    const uint32_t start = LeakAddressHash(address) & (kLeakTableSize - 1);
    for (uint32_t probe = 0; probe < kLeakMaxProbe; ++probe) {
        LeakEntry& entry = sLeakTable[(start + probe) & (kLeakTableSize - 1)];
        if (entry.state == kLeakEmpty) {
            return -1;
        }
        if (entry.state == kLeakUsed && entry.address == address) {
            return (int32_t)((start + probe) & (kLeakTableSize - 1));
        }
    }
    return -1;
}

static int32_t LeakFindInsertEntryLocked(uintptr_t address) {
    const uint32_t start = LeakAddressHash(address) & (kLeakTableSize - 1);
    int32_t deleted = -1;
    for (uint32_t probe = 0; probe < kLeakMaxProbe; ++probe) {
        const uint32_t index = (start + probe) & (kLeakTableSize - 1);
        LeakEntry& entry = sLeakTable[index];
        if (entry.state == kLeakEmpty) {
            return deleted >= 0 ? deleted : (int32_t)index;
        }
        if (entry.state == kLeakDeleted && deleted < 0) {
            deleted = (int32_t)index;
        } else if (entry.state == kLeakUsed && entry.address == address) {
            return (int32_t)index;
        }
    }
    return deleted;
}

static void LeakSiteAddLocked(int32_t siteIndex, uint64_t bytes) {
    if (siteIndex < 0) {
        return;
    }
    LeakSite& site = sLeakSites[siteIndex];
    site.liveBytes += bytes;
    ++site.liveCount;
    if (bytes >= kLeakBigThreshold) {
        site.bigLiveBytes += bytes;
        ++site.bigLiveCount;
        site.churnBytes += bytes;
        ++site.churnCount;
        if (site.churnCount == 1) {
            site.churnMin = bytes;
            site.churnMax = bytes;
        } else {
            if (bytes < site.churnMin) {
                site.churnMin = bytes;
            }
            if (bytes > site.churnMax) {
                site.churnMax = bytes;
            }
        }
    }
}

static void LeakSiteRemoveLocked(int32_t siteIndex, uint64_t bytes) {
    if (siteIndex < 0) {
        return;
    }
    LeakSite& site = sLeakSites[siteIndex];
    site.liveBytes = site.liveBytes >= bytes ? site.liveBytes - bytes : 0;
    if (site.liveCount != 0) {
        --site.liveCount;
    }
    if (bytes >= kLeakBigThreshold) {
        site.bigLiveBytes = site.bigLiveBytes >= bytes ? site.bigLiveBytes - bytes : 0;
        if (site.bigLiveCount != 0) {
            --site.bigLiveCount;
        }
    }
}

static bool LeakAddLocked(uintptr_t address, uint64_t bytes, const uint32_t key[3]) {
    const int32_t entryIndex = LeakFindInsertEntryLocked(address);
    if (entryIndex < 0) {
        ++sLeakDropped;
        return false;
    }
    const int32_t siteIndex = LeakFindSiteLocked(key, true);
    if (siteIndex < 0) {
        ++sLeakDropped;
        return false;
    }

    LeakEntry& entry = sLeakTable[entryIndex];
    entry.address = address;
    entry.bytes = bytes;
    entry.key[0] = key[0];
    entry.key[1] = key[1];
    entry.key[2] = key[2];
    entry.site = siteIndex;
    entry.state = kLeakUsed;
    LeakSiteAddLocked(siteIndex, bytes);
    ++sLeakTracked;
    return true;
}

static void LeakRemoveLocked(uintptr_t address) {
    if (address == 0) {
        return;
    }
    const int32_t entryIndex = LeakFindEntryLocked(address);
    if (entryIndex < 0) {
        return;
    }
    LeakEntry& entry = sLeakTable[entryIndex];
    LeakSiteRemoveLocked(entry.site, entry.bytes);
    entry.address = 0;
    entry.bytes = 0;
    entry.site = -1;
    entry.state = kLeakDeleted;
    if (sLeakTracked != 0) {
        --sLeakTracked;
    }
}

// Called with the caller's reentrancy flag held, so the real allocator is not tracked. Allocates
// before LeakLock is taken, so the tracker lock is never held across newlib's malloc lock.
static bool LeakTablesReady() {
    if (!WIIU_LEAK_TRACKER) {
        return false;
    }
    if (sLeakTablesState == 2) {
        return true;
    }
    if (!__sync_bool_compare_and_swap(&sLeakTablesState, 0, 1)) {
        return false;
    }
    LeakEntry* table = (LeakEntry*)__real_memalign(64, sizeof(LeakEntry) * kLeakTableSize);
    LeakSite* sites = (LeakSite*)__real_memalign(64, sizeof(LeakSite) * kLeakSiteTableSize);
    if (table == nullptr || sites == nullptr) {
        sLeakTablesState = 3;
        return false;
    }
    memset(table, 0, sizeof(LeakEntry) * kLeakTableSize);
    memset(sites, 0, sizeof(LeakSite) * kLeakSiteTableSize);
    sLeakTable = table;
    sLeakSites = sites;
    __sync_synchronize();
    sLeakTablesState = 2;
    return true;
}

// SoH holds more than 262144 live allocations, which filled the table in soh923m4. Every block of
// 64 KB or more is tracked; smaller ones are sampled 1-in-16 by address, so a free always looks up
// the same decision. Small-block LEAK bytes/counts are therefore 1/16 of the real figures.
static bool LeakSampled(uintptr_t address, uint64_t bytes) {
    return bytes >= 65536u || (LeakAddressHash(address) & 15u) == 0u;
}

static void LeakRecord(uintptr_t address, uint64_t bytes, const uint32_t key[3]) {
    if (address == 0 || !LeakSampled(address, bytes) || !LeakTablesReady()) {
        return;
    }
    LeakLock();
    LeakAddLocked(address, bytes, key);
    LeakUnlock();
}

static void LeakForget(uintptr_t address) {
    if (address == 0 || !LeakTablesReady()) {
        return;
    }
    LeakLock();
    LeakRemoveLocked(address);
    LeakUnlock();
}

static void LeakReplace(uintptr_t oldAddress, uintptr_t newAddress, uint64_t bytes, const uint32_t key[3],
                        bool removeOld) {
    if (!LeakTablesReady()) {
        return;
    }
    LeakLock();
    if (removeOld) {
        LeakRemoveLocked(oldAddress);
    }
    if (newAddress != 0 && LeakSampled(newAddress, bytes)) {
        LeakAddLocked(newAddress, bytes, key);
    }
    LeakUnlock();
}

static void LeakEmitReport() {
    if (!WIIU_LEAK_TRACKER) {
        return;
    }
    if (sLeakTablesState != 2) {
        ::Ship::WiiU::Watchdog::Emit("LEAK: tables not ready state=%u\n", sLeakTablesState);
        return;
    }
    LeakTop top[kLeakTopCount] = {};
    LeakChurnTop churnTop[kLeakChurnTopCount] = {};
    uint32_t topCount = 0;
    uint32_t churnTopCount = 0;
    uint32_t tracked = 0;
    uint32_t dropped = 0;
    uint64_t bigLiveBytes = 0;
    uint32_t bigLiveCount = 0;

    if (!LeakTryLock()) {
        return;
    }

    tracked = sLeakTracked;
    dropped = sLeakDropped;
    for (uint32_t index = 0; index < kLeakSiteTableSize; ++index) {
        LeakSite& site = sLeakSites[index];
        const uint64_t growth = site.liveBytes > site.previousBytes ? site.liveBytes - site.previousBytes : 0;
        if (site.used && site.liveCount != 0 && growth != 0) {
            uint32_t insertAt = topCount;
            for (uint32_t topIndex = 0; topIndex < topCount; ++topIndex) {
                if (growth > top[topIndex].growth) {
                    insertAt = topIndex;
                    break;
                }
            }
            if (insertAt < kLeakTopCount) {
                if (topCount < kLeakTopCount) {
                    ++topCount;
                }
                for (uint32_t topIndex = topCount - 1; topIndex > insertAt; --topIndex) {
                    top[topIndex] = top[topIndex - 1];
                }
                top[insertAt].key[0] = site.key[0];
                top[insertAt].key[1] = site.key[1];
                top[insertAt].key[2] = site.key[2];
                top[insertAt].bytes = site.liveBytes;
                top[insertAt].count = site.liveCount;
                top[insertAt].growth = growth;
            }
        }
        if (site.used) {
            bigLiveBytes += site.bigLiveBytes;
            bigLiveCount += site.bigLiveCount;

            if (site.churnBytes != 0) {
                uint32_t insertAt = churnTopCount;
                for (uint32_t topIndex = 0; topIndex < churnTopCount; ++topIndex) {
                    if (site.churnBytes > churnTop[topIndex].bytes) {
                        insertAt = topIndex;
                        break;
                    }
                }
                if (insertAt < kLeakChurnTopCount) {
                    if (churnTopCount < kLeakChurnTopCount) {
                        ++churnTopCount;
                    }
                    for (uint32_t topIndex = churnTopCount - 1; topIndex > insertAt; --topIndex) {
                        churnTop[topIndex] = churnTop[topIndex - 1];
                    }
                    churnTop[insertAt].key[0] = site.key[0];
                    churnTop[insertAt].key[1] = site.key[1];
                    churnTop[insertAt].key[2] = site.key[2];
                    churnTop[insertAt].bytes = site.churnBytes;
                    churnTop[insertAt].count = site.churnCount;
                    churnTop[insertAt].min = site.churnMin;
                    churnTop[insertAt].max = site.churnMax;
                }
            }

            site.previousBytes = site.liveBytes;
            site.churnBytes = 0;
            site.churnCount = 0;
            site.churnMin = 0;
            site.churnMax = 0;
        }
    }
    LeakUnlock();

    for (uint32_t index = 0; index < topCount; ++index) {
        Ship::WiiU::Watchdog::Emit(
            "LEAK: key=0x%08X,0x%08X,0x%08X bytes=%llu count=%u growth=%llu\n", top[index].key[0],
            top[index].key[1], top[index].key[2], (unsigned long long)top[index].bytes, top[index].count,
            (unsigned long long)top[index].growth);
    }
    Ship::WiiU::Watchdog::Emit("LEAKBIG: live=%llu count=%u\n", (unsigned long long)bigLiveBytes, bigLiveCount);
    for (uint32_t index = 0; index < churnTopCount; ++index) {
        Ship::WiiU::Watchdog::Emit(
            "CHURN: key=0x%08X,0x%08X,0x%08X bytes=%llu count=%u min=%llu max=%llu\n", churnTop[index].key[0],
            churnTop[index].key[1], churnTop[index].key[2], (unsigned long long)churnTop[index].bytes,
            churnTop[index].count, (unsigned long long)churnTop[index].min, (unsigned long long)churnTop[index].max);
    }
    Ship::WiiU::Watchdog::Emit("LEAK: tracked=%u dropped=%u sample=1/16 below 64KB\n", tracked, dropped);
}


static uint64_t ProbeLargestFree() {
    static const size_t kProbeMax = 64u * 1024u * 1024u;
    static const size_t kProbeMin = 4u * 1024u;
    uint64_t largest = 0;

    for (size_t probe = kProbeMax;; probe >>= 1) {
        void* result = __real_malloc(probe);
        if (result != nullptr) {
            if (largest == 0) {
                largest = probe;
            }
            __real_free(result);
        }
        if (probe == kProbeMin) {
            break;
        }
    }
    return largest;
}

#endif // WIIU_DIAGNOSTICS (leak tracker + ProbeLargestFree)

#if WIIU_DIAGNOSTICS
static void LeakEmitAllocFailure(const char* function, uint64_t bytes, uint64_t alignment,
                                 const uint32_t key[3]) {
    const struct mallinfo heapInfo = mallinfo();
    const uint64_t largest = ProbeLargestFree();
    Ship::WiiU::Watchdog::Emit(
        "ALLOCFAIL: fn=%s size=%llu align=%llu key=0x%08X,0x%08X,0x%08X arena=%u used=%u largest=%llu bigFree=%u "
        "bigLargest=%u\n",
        function, (unsigned long long)bytes, (unsigned long long)alignment, key[0], key[1], key[2],
        (uint32_t)heapInfo.arena, (uint32_t)heapInfo.uordblks, (unsigned long long)largest, BigHeapFreeBytes(),
        BigHeapLargestFree());
}

static void LeakEmitAllocFailureIfUnreentrant(LeakThreadState* state, const char* function, uint64_t bytes,
                                              uint64_t alignment, const uint32_t key[3]) {
    const bool acquired = __sync_lock_test_and_set(&state->reentrant, 1) == 0;
    LeakEmitAllocFailure(function, bytes, alignment, key);
    if (acquired) {
        __sync_lock_release(&state->reentrant);
    }
}

#endif // WIIU_DIAGNOSTICS

// ---- Large-block heap -------------------------------------------------------------------
// With the texture pack, newlib's arena hit its 943 MB ceiling while only ~555 MB was in use
// (soh923l): every pack texture passes through four 64 KB-1 MB blocks (O2R read buffer, init-data
// copy, decoded image, GX2 surface), transient and long-lived interleaved with small long-lived
// objects, and the holes left behind never fit the next texture. Blocks of 64 KB and more now live
// in their own Cafe OS expanded heap, carved from the arena on the first allocation while it is
// still empty, so small objects can never split the space between large ones. Either side falls
// back to the other when it runs out; frees are routed by address.
static const uint32_t kBigBlockMin = 64u * 1024u;
static const uint32_t kBigHeapSize = 560u * 1024u * 1024u;
static MEMHeapHandle sBigHeap = nullptr;
static uintptr_t sBigHeapBase = 0;
static uintptr_t sBigHeapEnd = 0;
static volatile uint32_t sBigHeapState = 0; // 0 = not tried, 1 = initialising, 2 = ready, 3 = failed

static void BigHeapInit() {
    if (sBigHeapState >= 2 || !__sync_bool_compare_and_swap(&sBigHeapState, 0, 1)) {
        return;
    }
    void* base = __real_memalign(64, kBigHeapSize);
    if (base != nullptr) {
        MEMHeapHandle heap = MEMCreateExpHeapEx(base, kBigHeapSize, MEM_HEAP_FLAG_USE_LOCK);
        if (heap != nullptr) {
            sBigHeap = heap;
            sBigHeapBase = (uintptr_t)base;
            sBigHeapEnd = (uintptr_t)base + kBigHeapSize;
            __sync_synchronize();
            sBigHeapState = 2;
            return;
        }
        __real_free(base);
    }
    sBigHeapState = 3;
}

static inline bool BigHeapOwns(const void* pointer) {
    const uintptr_t address = (uintptr_t)pointer;
    return address >= sBigHeapBase && address < sBigHeapEnd;
}

static void* BigHeapAlloc(size_t size, size_t alignment) {
    if (sBigHeapState != 2 || size == 0 || size > 0x7FFFFFFFu) {
        return nullptr;
    }
    if (alignment < 16) {
        alignment = 16;
    }
    return MEMAllocFromExpHeapEx(sBigHeap, (uint32_t)size, (int)alignment);
}

static void* BigRouteAlloc(size_t size, size_t alignment) {
    BigHeapInit();
    void* result = nullptr;
    if (size >= kBigBlockMin) {
        result = BigHeapAlloc(size, alignment);
    }
    if (result == nullptr) {
        result = alignment <= 8 ? __real_malloc(size) : __real_memalign(alignment, size);
    }
    if (result == nullptr && size < kBigBlockMin) {
        result = BigHeapAlloc(size, alignment);
    }
    return result;
}

static void BigRouteFree(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
    if (BigHeapOwns(pointer)) {
        MEMFreeToExpHeap(sBigHeap, pointer);
    } else {
        __real_free(pointer);
    }
}

static void* BigRouteCalloc(size_t count, size_t size) {
    if (size != 0 && count > (size_t)-1 / size) {
        return nullptr;
    }
    const size_t bytes = count * size;
    void* result = BigRouteAlloc(bytes, 0);
    if (result != nullptr) {
        memset(result, 0, bytes);
    }
    return result;
}

static void* BigRouteRealloc(void* pointer, size_t size) {
    if (pointer == nullptr) {
        return BigRouteAlloc(size, 0);
    }
    if (size == 0) {
        BigRouteFree(pointer);
        return nullptr;
    }
    const bool fromBig = BigHeapOwns(pointer);
    if (!fromBig && size < kBigBlockMin) {
        return __real_realloc(pointer, size);
    }
    const size_t oldSize = fromBig ? MEMGetSizeForMBlockExpHeap(pointer) : malloc_usable_size(pointer);
    void* result = BigRouteAlloc(size, 0);
    if (result == nullptr) {
        return nullptr; // the old block stays valid, as realloc requires
    }
    memcpy(result, pointer, oldSize < size ? oldSize : size);
    BigRouteFree(pointer);
    return result;
}

static uint32_t BigHeapFreeBytes() {
    return sBigHeapState == 2 ? MEMGetTotalFreeSizeForExpHeap(sBigHeap) : 0;
}

static uint32_t BigHeapLargestFree() {
    return sBigHeapState == 2 ? MEMGetAllocatableSizeForExpHeapEx(sBigHeap, 16) : 0;
}

} // namespace

extern "C" void wiiu_get_perf_big_heap(uint32_t* free_bytes, uint32_t* largest_free_bytes) {
    if (free_bytes != nullptr) {
        *free_bytes = BigHeapFreeBytes();
    }
    if (largest_free_bytes != nullptr) {
        *largest_free_bytes = BigHeapLargestFree();
    }
}

static int RoutePosixMemalign(void** pointer, size_t alignment, size_t size) {
    if (pointer == nullptr) {
        return EINVAL;
    }
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void* result = BigRouteAlloc(size, alignment);
    if (result == nullptr) {
        return ENOMEM;
    }
    *pointer = result;
    return 0;
}

extern "C" {
#if WIIU_DIAGNOSTICS
void* __wrap_malloc(size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    if (__sync_lock_test_and_set(&state->reentrant, 1) != 0) {
        void* result = BigRouteAlloc(size, 0);
        if (result == nullptr) {
            const uint32_t key[3] = { 0, 0, 0 };
            LeakEmitAllocFailure("malloc", size, 0, key);
        }
        return result;
    }
    uint32_t key[3];
    LeakCaptureKey(key, state->newDepth != 0);
    void* result = BigRouteAlloc(size, 0);
    if (result == nullptr) {
        LeakEmitAllocFailure("malloc", size, 0, key);
    } else {
        LeakRecord((uintptr_t)result, size, key);
    }
    __sync_lock_release(&state->reentrant);
    return result;
}

void __wrap_free(void* pointer) {
    LeakThreadState* state = LeakCurrentThreadState();
    if (__sync_lock_test_and_set(&state->reentrant, 1) != 0) {
        BigRouteFree(pointer);
        return;
    }
    // Forget first: once freed, another thread can be handed this address and record it,
    // and a late forget would then delete the new record.
    LeakForget((uintptr_t)pointer);
    BigRouteFree(pointer);
    __sync_lock_release(&state->reentrant);
}

void* __wrap_calloc(size_t count, size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    if (__sync_lock_test_and_set(&state->reentrant, 1) != 0) {
        void* result = BigRouteCalloc(count, size);
        if (result == nullptr) {
            const uint32_t key[3] = { 0, 0, 0 };
            LeakEmitAllocFailure("calloc", (uint64_t)count * (uint64_t)size, 0, key);
        }
        return result;
    }
    uint32_t key[3];
    LeakCaptureKey(key, state->newDepth != 0);
    void* result = BigRouteCalloc(count, size);
    const uint64_t bytes = (uint64_t)count * (uint64_t)size;
    if (result == nullptr) {
        LeakEmitAllocFailure("calloc", bytes, 0, key);
    } else {
        LeakRecord((uintptr_t)result, bytes, key);
    }
    __sync_lock_release(&state->reentrant);
    return result;
}

void* __wrap_realloc(void* pointer, size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    if (__sync_lock_test_and_set(&state->reentrant, 1) != 0) {
        void* result = BigRouteRealloc(pointer, size);
        if (result == nullptr && size != 0) {
            const uint32_t key[3] = { 0, 0, 0 };
            LeakEmitAllocFailure("realloc", size, 0, key);
        }
        return result;
    }
    uint32_t key[3];
    LeakCaptureKey(key, state->newDepth != 0);
    void* result = BigRouteRealloc(pointer, size);
    if (result != nullptr || size == 0) {
        LeakReplace((uintptr_t)pointer, (uintptr_t)result, size, key, true);
    } else {
        LeakEmitAllocFailure("realloc", size, 0, key);
    }
    __sync_lock_release(&state->reentrant);
    return result;
}

void* __wrap_memalign(size_t alignment, size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    if (__sync_lock_test_and_set(&state->reentrant, 1) != 0) {
        void* result = BigRouteAlloc(size, alignment);
        if (result == nullptr) {
            const uint32_t key[3] = { 0, 0, 0 };
            LeakEmitAllocFailure("memalign", size, alignment, key);
        }
        return result;
    }
    uint32_t key[3];
    LeakCaptureKey(key, state->newDepth != 0);
    void* result = BigRouteAlloc(size, alignment);
    if (result == nullptr) {
        LeakEmitAllocFailure("memalign", size, alignment, key);
    } else {
        LeakRecord((uintptr_t)result, size, key);
    }
    __sync_lock_release(&state->reentrant);
    return result;
}

int __wrap_posix_memalign(void** pointer, size_t alignment, size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    if (__sync_lock_test_and_set(&state->reentrant, 1) != 0) {
        const int result = RoutePosixMemalign(pointer, alignment, size);
        if (result != 0) {
            const uint32_t key[3] = { 0, 0, 0 };
            LeakEmitAllocFailure("posix_memalign", size, alignment, key);
        }
        return result;
    }
    uint32_t key[3];
    LeakCaptureKey(key, state->newDepth != 0);
    const int result = RoutePosixMemalign(pointer, alignment, size);
    if (result == 0 && pointer != nullptr) {
        LeakRecord((uintptr_t)*pointer, size, key);
    } else if (result != 0) {
        LeakEmitAllocFailure("posix_memalign", size, alignment, key);
    }
    __sync_lock_release(&state->reentrant);
    return result;
}

void* __wrap_aligned_alloc(size_t alignment, size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    if (__sync_lock_test_and_set(&state->reentrant, 1) != 0) {
        void* result = BigRouteAlloc(size, alignment);
        if (result == nullptr) {
            const uint32_t key[3] = { 0, 0, 0 };
            LeakEmitAllocFailure("aligned_alloc", size, alignment, key);
        }
        return result;
    }
    uint32_t key[3];
    LeakCaptureKey(key, state->newDepth != 0);
    void* result = BigRouteAlloc(size, alignment);
    if (result == nullptr) {
        LeakEmitAllocFailure("aligned_alloc", size, alignment, key);
    } else {
        LeakRecord((uintptr_t)result, size, key);
    }
    __sync_lock_release(&state->reentrant);
    return result;
}



void* __wrap__Znwj(size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    uint32_t key[3];
    LeakCaptureKey(key, false);
    ++state->newDepth;
    void* result;
    try {
        result = __real__Znwj(size);
    } catch (const std::bad_alloc&) {
        --state->newDepth;
        LeakEmitAllocFailureIfUnreentrant(state, "new", size, 0, key);
        throw;
    }
    --state->newDepth;
    if (result == nullptr) {
        LeakEmitAllocFailureIfUnreentrant(state, "new", size, 0, key);
        throw std::bad_alloc();
    }
    return result;
}

void* __wrap__Znaj(size_t size) {
    LeakThreadState* state = LeakCurrentThreadState();
    uint32_t key[3];
    LeakCaptureKey(key, false);
    ++state->newDepth;
    void* result;
    try {
        result = __real__Znaj(size);
    } catch (const std::bad_alloc&) {
        --state->newDepth;
        LeakEmitAllocFailureIfUnreentrant(state, "new[]", size, 0, key);
        throw;
    }
    --state->newDepth;
    if (result == nullptr) {
        LeakEmitAllocFailureIfUnreentrant(state, "new[]", size, 0, key);
        throw std::bad_alloc();
    }
    return result;
}

#else

// Same semantics as the diagnostics build minus the logging: the malloc family returns nullptr
// and operator new throws std::bad_alloc, so callers that handle failure still can. An uncaught
// bad_alloc ends in the terminate handler's OSFatal.
void* __wrap_malloc(size_t size) {
    return BigRouteAlloc(size, 0);
}

void __wrap_free(void* pointer) {
    BigRouteFree(pointer);
}

void* __wrap_calloc(size_t count, size_t size) {
    return BigRouteCalloc(count, size);
}

void* __wrap_realloc(void* pointer, size_t size) {
    return BigRouteRealloc(pointer, size);
}

void* __wrap_memalign(size_t alignment, size_t size) {
    return BigRouteAlloc(size, alignment);
}

int __wrap_posix_memalign(void** pointer, size_t alignment, size_t size) {
    return RoutePosixMemalign(pointer, alignment, size);
}

void* __wrap_aligned_alloc(size_t alignment, size_t size) {
    return BigRouteAlloc(size, alignment);
}

void* __wrap__Znwj(size_t size) {
    void* result = __real__Znwj(size);
    if (result == nullptr) {
        throw std::bad_alloc();
    }
    return result;
}

void* __wrap__Znaj(size_t size) {
    void* result = __real__Znaj(size);
    if (result == nullptr) {
        throw std::bad_alloc();
    }
    return result;
}

#endif // WIIU_DIAGNOSTICS
}

static int wiiu_log_open(struct _reent*, void*, const char*, int, int) {
    return -1;
}

static int wiiu_log_close(struct _reent*, void*) {
    return 0;
}

static int wiiu_log_fstat(struct _reent*, void*, struct stat* st) {
    memset(st, 0, sizeof(*st));
    return 0;
}

static ssize_t wiiu_log_discard(struct _reent*, void*, const char*, size_t len) {
    return len;
}

static const devoptab_t dotab_devnull = {
    .name = "devnull_wiiu",
    .open_r = wiiu_log_open,
    .close_r = wiiu_log_close,
    .write_r = wiiu_log_discard,
    .fstat_r = wiiu_log_fstat,
};

extern "C" {
void __real___cxa_throw(void* exception, std::type_info* typeInfo, void (*destructor)(void*))
    __attribute__((noreturn));
extern void __init(void);

static void RestoreStdioDevoptab() {
#if WIIU_DIAGNOSTICS
    // In the linked ELF, devoptab_list[STD_IN/OUT/ERR] default to &dotab_stdnull.
    // Its measured open_r, close_r, read_r, and fstat_r slots are all NULL, so
    // restoring the original entries would hand those NULL slots back during teardown.
    devoptab_list[STD_OUT] = &dotab_devnull;
    devoptab_list[STD_ERR] = &dotab_devnull;
#endif
}

// Every SohGui::RegisterPopup OK handler calls exit(). On Cafe OS that unwinds atexit
// handlers and static destructors while the GX2 context and ProcUI are still live, and
// something in that teardown never returns - the console hangs and needs a power cycle.
// Same class of problem as abort() in the terminate handler, and the same remedy: do the
// small amount of teardown that is safe, then _Exit so the system reclaims the title
// normally and the Aroma plugins survive.
void __real_exit(int status) __attribute__((noreturn));
void __wrap_exit(int status) {
    Ship::WiiU::Watchdog::StopProfiler();
#if WIIU_DIAGNOSTICS
    Ship::WiiU::Watchdog::Emit("EXIT: exit(%d) - bypassing destructor teardown\n", status);
#endif
    KPADShutdown();
    RestoreStdioDevoptab();
#if WIIU_DIAGNOSTICS
    WHBLogUdpDeinit();
#endif
    _Exit(status);
}

void __wrap___cxa_throw(void* exception, std::type_info* typeInfo, void (*destructor)(void*)) {
#if WIIU_DIAGNOSTICS
    // One frame is not enough: __cxa_throw is called BY std::__throw_out_of_range, so
    // __builtin_return_address(0) only ever names that helper. Walk the PowerPC
    // back-chain instead - r1 points at a frame whose first word is the caller's frame
    // and whose second word is that frame's saved LR - to get the real call path.
    //
    // A constant tied to this translation unit went stale on every rebuild:
    // 0x049057c4 -> 0x0490581c -> 0x04904fc8. __init only moves if the crt layout in
    // the linker script moves, so derive the bias from its fixed head-of-.text address.
    const uint32_t bias = (uint32_t)(uintptr_t)&__init - 0x02000080u;

    uint32_t* sp;
    __asm__ volatile("mr %0, 1" : "=r"(sp));

    char line[512];
    int n = snprintf(line, sizeof(line), "CXX: throw type=%s bias=0x%08X frames:", typeInfo->name(), bias);
    for (int depth = 0; depth < 8 && sp != nullptr && n < (int)sizeof(line) - 16; ++depth) {
        uint32_t* next = (uint32_t*)sp[0];
        if (next <= sp || (uintptr_t)next & 3u) {
            break;
        }
        const uint32_t lr = next[1];
        if (lr > bias) {
            n += snprintf(line + n, sizeof(line) - n, " 0x%08X", lr - bias);
        }
        sp = next;
    }
    Ship::WiiU::Watchdog::Emit("%s\n", line);
#endif
    __real___cxa_throw(exception, typeInfo, destructor);
}
}

namespace Ship {
namespace WiiU {

static bool hasVpad = false;
static VPADReadError vpadError;
static VPADStatus vpadStatus;

static bool hasKpad[4] = { false };
static KPADError kpadError[4] = { KPAD_ERROR_OK };
static KPADStatus kpadStatus[4];

// This is the only storage used by the exception path.  It must remain independent of
// the heap, locks, sockets, and the game's renderer: OSFatal owns the last step.
static char sExceptionMessage[768];
static const char sExceptionHexDigits[] = "0123456789ABCDEF";
static const char* const sExceptionGprNames[] = {
    "GPR0",  "GPR1",  "GPR2",  "GPR3",  "GPR4",  "GPR5",  "GPR6",
    "GPR7",  "GPR8",  "GPR9",  "GPR10", "GPR11", "GPR12",
};

static char* AppendExceptionText(char* destination, const char* source) {
    while (*source != '\0') {
        *destination++ = *source++;
    }
    return destination;
}

static char* AppendExceptionHex(char* destination, uint32_t value) {
    *destination++ = '0';
    *destination++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        *destination++ = sExceptionHexDigits[(value >> shift) & 0xF];
    }
    return destination;
}

static char* AppendExceptionRegister(char* destination, const char* name, uint32_t value) {
    destination = AppendExceptionText(destination, name);
    *destination++ = '=';
    destination = AppendExceptionHex(destination, value);
    *destination++ = ' ';
    return destination;
}

#if WIIU_DIAGNOSTICS
static void EmitExceptionMessage(const char* typeName, const OSContext* context) {
    // Emit formats into a 384-byte stack-local buffer, so sequential calls keep their own
    // text. Emit the register evidence before OSGetSymbolName: symbol lookup may stall or
    // fault in the exception context. The GPRs still reach the screen via OSFatal.
    if (context == nullptr) {
        Watchdog::Emit("EXC: type=%s context=NULL\n", typeName);
        return;
    }

    uint32_t* sp = (uint32_t*)(uintptr_t)context->gpr[1];
    uint32_t caller = 0;
    do {
        if (sp == nullptr || (uintptr_t)sp & 3u) {
            break;
        }
        uint32_t* next = (uint32_t*)sp[0];
        if (next <= sp || (uintptr_t)next & 3u) {
            break;
        }
        caller = next[1];
    } while (false);

    Watchdog::Emit("EXC: %s srr0=0x%08X dar=0x%08X dsisr=0x%08X srr1=0x%08X lr=0x%08X r1=0x%08X core=%u\n",
                   typeName, context->srr0, context->dar, context->dsisr, context->srr1, context->lr,
                   context->gpr[1], (unsigned int)OSGetCoreId());

    char lrSymbol[64] = {};
    char callerSymbol[64] = {};
    OSGetSymbolName(context->lr, lrSymbol, sizeof(lrSymbol));
    lrSymbol[sizeof(lrSymbol) - 1] = '\0';
    if (caller != 0) {
        OSGetSymbolName(caller, callerSymbol, sizeof(callerSymbol));
        callerSymbol[sizeof(callerSymbol) - 1] = '\0';
    }
    Watchdog::Emit("EXC-SYM: lr=0x%08X %s caller=0x%08X %s\n", context->lr, lrSymbol, caller, callerSymbol);
}
#endif

static void FormatExceptionMessage(const char* typeName, const OSContext* context) {
    char* destination = sExceptionMessage;
    destination = AppendExceptionText(destination, "Wii U ");
    destination = AppendExceptionText(destination, typeName);
    destination = AppendExceptionText(destination, " exception\n");

    if (context == nullptr) {
        destination = AppendExceptionText(destination, "exception context unavailable\n");
        *destination = '\0';
        return;
    }

    destination = AppendExceptionRegister(destination, "SRR0", context->srr0);
    destination = AppendExceptionRegister(destination, "SRR1", context->srr1);
    *destination++ = '\n';
    destination = AppendExceptionRegister(destination, "DAR", context->dar);
    destination = AppendExceptionRegister(destination, "DSISR", context->dsisr);
    *destination++ = '\n';
    destination = AppendExceptionRegister(destination, "LR", context->lr);
    *destination++ = '\n';

    // r0-r12 cover the volatile call state, stack pointer, TOC, and argument registers.
    for (int gpr = 0; gpr <= 12; ++gpr) {
        destination = AppendExceptionRegister(destination, sExceptionGprNames[gpr], context->gpr[gpr]);
        if (gpr == 3 || gpr == 6 || gpr == 9 || gpr == 12) {
            *destination++ = '\n';
        }
    }
    *destination = '\0';
}

static BOOL FatalException(const char* typeName, OSContext* context) {
    FormatExceptionMessage(typeName, context);
#if WIIU_DIAGNOSTICS
    EmitExceptionMessage(typeName, context);
#endif
    OSFatal(sExceptionMessage);
    return FALSE;
}

static BOOL DsiExceptionCallback(OSContext* context) {
    return FatalException("DSI", context);
}

static BOOL IsiExceptionCallback(OSContext* context) {
    return FatalException("ISI", context);
}

static BOOL ProgramExceptionCallback(OSContext* context) {
    return FatalException("PROGRAM", context);
}

static BOOL MachineCheckExceptionCallback(OSContext* context) {
    return FatalException("MACHINE_CHECK", context);
}

static BOOL AlignmentExceptionCallback(OSContext* context) {
    return FatalException("ALIGNMENT", context);
}

static BOOL FloatingPointExceptionCallback(OSContext* context) {
    return FatalException("FLOATING_POINT", context);
}

// All six types the previous socket-based version covered. ALIGNMENT in particular is
// worth keeping on PowerPC: this port reads little-endian archive data, and a misaligned
// or swapped access is a live failure mode here, not a theoretical one.
//
// OSSetExceptionCallbackEx(GLOBAL_ALL_CORES) replaces the old per-core OSSetExceptionCallback,
// which needed a thread spawned on each of the three cores to install itself.
static void InstallExceptionCallbacks() {
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, DsiExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, IsiExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, ProgramExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_MACHINE_CHECK,
                             MachineCheckExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT,
                             AlignmentExceptionCallback);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_FLOATING_POINT,
                             FloatingPointExceptionCallback);
}

#if WIIU_DIAGNOSTICS
static OSThread sCrashTestThread;
static uint8_t sCrashTestStack[4 * 1024] __attribute__((aligned(16)));

static int CrashTestMain(int, const char**) {
    OSSleepTicks(OSMillisecondsToTicks(1000));
    Watchdog::Emit("EXC: CRASHTEST triggering DSI\n");
    *(volatile uint32_t*)0xdeadc0d0 = 0xcafebabe;
    return 0;
}

static void ArmCrashTestIfRequested() {
    // Init has already chdir'd into /vol/external01/wiiu/apps/<shortName>, and "sd:" is not a
    // mount wut provides - use the real device path so a missing file means "not requested"
    // rather than "wrong path".
    FILE* marker = fopen("CRASHTEST", "rb");
    if (marker == nullptr) {
        return;
    }
    fclose(marker);

    Watchdog::Emit("EXC: CRASHTEST found; DSI scheduled in 1000ms\n");
    if (!OSCreateThread(&sCrashTestThread, CrashTestMain, 0, nullptr,
                        sCrashTestStack + sizeof(sCrashTestStack), sizeof(sCrashTestStack), 5,
                        (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU1)) {
        Watchdog::Emit("EXC: CRASHTEST thread creation failed\n");
        return;
    }
    OSSetThreadName(&sCrashTestThread, "Wii U crash test");
    OSResumeThread(&sCrashTestThread);
}
#endif // WIIU_DIAGNOSTICS

extern "C" {
void __wrap_abort() {
    // An assert() or abort() fired. printf() here only reaches the devoptab/UDP path, which is
    // exactly the channel that keeps failing on this platform - so write the fact to the SD card
    // too, next to the spdlog file, where it survives the freeze and the reboot that follows.
    printf("Abort called.\n");
    FILE* f = fopen("abort.log", "a");
    if (f) {
        fprintf(f, "abort() called - an assert fired. See the tail of the game log for the last\n"
                   "successful operation; the assert is in whatever ran immediately after it.\n");
        fflush(f);
        fclose(f);
    }
    // Also try to flush whatever spdlog still holds, so the game log is complete.
    fflush(NULL);
    // force a stack trace
    *(uint32_t*)0xdeadc0de = 0xcafebabe;
    while (1)
        ;
}

#if WIIU_DIAGNOSTICS
static ssize_t wiiu_log_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%*.*s", len, len, ptr);
    OSReport(buf);
    WHBLogWritef("%*.*s", len, len, ptr);
    return len;
}

static const devoptab_t dotab_stdout = {
    .name = "stdout_whb",
    .open_r = wiiu_log_open,
    .close_r = wiiu_log_close,
    .write_r = wiiu_log_write,
    .fstat_r = wiiu_log_fstat,
};
#endif
};

void Init(const std::string& shortName) {
    // Called twice now: once at the very top of InitOTR (so logging and the chdir are
    // up before ANY filesystem access) and once where upstream has always called it,
    // at the tail of RunExtract. Second call is a no-op.
    static bool sInitialised = false;
    if (sInitialised) {
        return;
    }
    sInitialised = true;

#if WIIU_DIAGNOSTICS
    WHBLogUdpInit();
    WHBLogPrint("Hello World!");

    devoptab_list[STD_OUT] = &dotab_stdout;
    devoptab_list[STD_ERR] = &dotab_stdout;
#endif

    // make sure the required folders exist
    mkdir("/vol/external01/wiiu/", 0755);
    mkdir("/vol/external01/wiiu/apps/", 0755);
    mkdir(("/vol/external01/wiiu/apps/" + shortName).c_str(), 0755);

    chdir(("/vol/external01/wiiu/apps/" + shortName).c_str());

    KPADInit();
    WPADEnableURCC(true);

    Watchdog::Start();
    InstallExceptionCallbacks();
#if WIIU_DIAGNOSTICS
    Watchdog::Emit("EXC: exception handlers installed successfully\n");
    ArmCrashTestIfRequested();
#endif
}

void Exit() {
    KPADShutdown();

    RestoreStdioDevoptab();
#if WIIU_DIAGNOSTICS
    WHBLogUdpDeinit();
#endif
}

void ThrowMissingOTR(const char* otrPath) {
    // TODO handle this better in the future
    OSFatal("Main OTR file not found!");
}

void ThrowInvalidOTR() {
    OSFatal("Invalid OTR files! Try regenerating them!");
}

void Update() {
    VPADRead(VPAD_CHAN_0, &vpadStatus, 1, &vpadError);
    if (vpadError == VPAD_READ_SUCCESS) {
        hasVpad = true;
    } else if (vpadError != VPAD_READ_NO_SAMPLES) {
        hasVpad = false;
    }

    for (int i = 0; i < 4; i++) {
        KPADReadEx((KPADChan)i, &kpadStatus[i], 1, &kpadError[i]);
        if (kpadError[i] == KPAD_ERROR_OK && kpadStatus[i].extensionType != 255) {
            hasKpad[i] = true;
        } else if (kpadError[i] != KPAD_ERROR_NO_SAMPLES) {
            hasKpad[i] = false;
        }
    }
}

VPADStatus* GetVPADStatus(VPADReadError* error) {
    *error = vpadError;
    return hasVpad ? &vpadStatus : nullptr;
}

KPADStatus* GetKPADStatus(WPADChan chan, KPADError* error) {
    *error = kpadError[chan];
    return hasKpad[chan] ? &kpadStatus[chan] : nullptr;
}

namespace Watchdog {

volatile uint32_t gSeq = 0;
volatile uint32_t gPhase = PH_NONE;
volatile uint32_t gSteps = 0;
volatile uint32_t gOpcode = 0;
volatile uint32_t gCmd = 0;
volatile uint32_t gFrame = 0;
char gDetail[128] = { 0 };
const char* volatile gDetailFmt = nullptr;
volatile uint32_t gDetailArgs[6] = { 0 };
char gTexturePath[128] = { 0 };
volatile uint32_t gTraceState = TRACE_OFF;
volatile uint32_t gTraceFramesRemaining = 0;
volatile uint32_t gTraceStepState = TRACE_STEPS_WAITING;
volatile uint32_t gTraceStepsRemaining = 0;
volatile uint32_t gAudioSeq = 0;
volatile uint32_t gAudioPhase = 0;
volatile uint32_t gTexBytes = 0;
volatile uint32_t gTexCount = 0;
volatile uint32_t gTexLiveBytes = 0;
volatile uint32_t gTexLiveCount = 0;
volatile uint32_t gLastTexPtr = 0;
volatile uint32_t gTextureCacheSize = 0;
volatile uint32_t gFreeTextureIdsSize = 0;
volatile uint32_t gOtrTextureCacheSize = 0;
volatile uint32_t gRawPointerByPathSize = 0;
volatile uint32_t gRawPointerByHashSize = 0;
volatile uint32_t gResourceCacheSize = 0;
volatile uint32_t gShaderProgramPoolSize = 0;
volatile uint32_t gFlipCount = 0;
volatile uint32_t gOtrCacheHits = 0;
volatile uint32_t gOtrCacheMisses = 0;
volatile uint32_t gOtrResourceManagerLookups = 0;
volatile uint32_t gDrawBufferHighWaterBytes = 0;
volatile uint32_t gGx2SlotWaits = 0;
volatile uint32_t gFrameTimingCount = 0;
volatile uint32_t gFrameTimingGpuUs = 0;
volatile uint32_t gFrameTimingCpuUs = 0;
volatile uint32_t gFrameTimingWaitUs = 0;
// Keep the periodic watchdog and explicitly requested diagnostics, but do not stream every
// watchdog Enter/Leave pair from the GX2 hot path.
volatile uint32_t gEventStream = 0;
volatile uint32_t gEmitOk = 0;
volatile uint32_t gEmitFail = 0;

#if WIIU_DIAGNOSTICS
// The profiler is deliberately a small, allocation-free open-addressed table. The watchdog
// samples the render thread while it is suspended, then does all table work only after the
// thread has been resumed.
static const uint32_t kProfileTableSize = 1024;
static const uint32_t kProfileTopPcCount = 24;
static const uint32_t kProfileTopLrCount = 12;
static const uint32_t kProfileEntriesPerLine = 6;
static const uint32_t kProfileSampleIntervalMs = 10;
static const uint32_t kProfileSamplesPerReport = 1000;
static const uint32_t kProfileSamplesPerWatchdogTick = WDOG_TICK_INTERVAL_MS / kProfileSampleIntervalMs;

struct ProfileBucket {
    uint32_t address;
    uint32_t count;
};

struct ProfileTop {
    uint32_t address;
    uint32_t count;
};

// Double-buffered: the alarm callback (interrupt context, game core) fills [sProfileActive];
// the watchdog flips the index every report and reads/clears the other set.
static ProfileBucket sPcProfile[2][kProfileTableSize];
static ProfileBucket sLrProfile[2][kProfileTableSize];
static volatile uint32_t sProfileActive = 0;
static volatile uint32_t sProfileCount[2] = { 0, 0 };
// "Game caller": for samples outside the game's .text, the first return address on the
// stack that IS game code - attributes system/GX2/lock time to the game function behind it.
static ProfileBucket sGcProfile[2][kProfileTableSize];
// "Allocator caller": game returns hidden behind allocator code, recorded only for samples
// interrupted in an allocator or whose stack walk had to skip allocator returns.
static ProfileBucket sAcProfile[2][kProfileTableSize];
// "Blocked in": when the game thread is NOT the one running on its core (the core is idle or
// running something else), the game thread is waiting. Its saved context is valid then, so
// walking its own stack names the game function it is blocked under.
static ProfileBucket sBkProfile[2][kProfileTableSize];
// Interrupted OSThread pointers, validated only by their tag in the alarm callback.
static ProfileBucket sThProfile[2][kProfileTableSize];
static OSAlarm sProfileAlarm;
static ProfileTop sTopPcs[kProfileTopPcCount];
static ProfileTop sTopLrs[kProfileTopLrCount];
static uint32_t sProfileSampleCount = 0;

// UNICAST, not broadcast. Broadcast is what died at 322 datagrams/second on 2026-09-13 and
// took every diagnostic line with it, which then read as a PowerPC-wide wedge for two
// sessions. This is the workstation running wiiu/udplog.py; udplog.py binds 0.0.0.0 so it
// receives unicast without any change. If the listener moves, this constant moves with it.
static const uint32_t kLogHostAddr = 0x0A0104ABu; // 10.1.4.171

void RecordGX2Wait(uint32_t microseconds) {
    __sync_fetch_and_add(&gFrameTimingWaitUs, microseconds);
}

void RecordGX2SlotWait(uint32_t microseconds) {
    __sync_fetch_and_add(&gGx2SlotWaits, 1);
    RecordGX2Wait(microseconds);
}

void RecordFrameTiming(uint32_t gpuMicroseconds, uint32_t cpuMicroseconds) {
    __sync_fetch_and_add(&gFrameTimingGpuUs, gpuMicroseconds);
    __sync_fetch_and_add(&gFrameTimingCpuUs, cpuMicroseconds);
    __sync_synchronize();
    __sync_fetch_and_add(&gFrameTimingCount, 1);
}

static std::atomic<uint32_t> sHitchResourceFactoryMicroseconds{ 0 };
static std::atomic<uint32_t> sHitchTextureUploadMicroseconds{ 0 };
static std::atomic<uint32_t> sHitchTextureUploadCount{ 0 };
static std::atomic<uint32_t> sHitchShaderProgramCount{ 0 };
static uint32_t sHitchBurstLines = 0;

void RecordResourceFactory(uint32_t microseconds) {
    sHitchResourceFactoryMicroseconds.fetch_add(microseconds, std::memory_order_relaxed);
}

void RecordTextureUpload(uint32_t microseconds) {
    sHitchTextureUploadMicroseconds.fetch_add(microseconds, std::memory_order_relaxed);
    sHitchTextureUploadCount.fetch_add(1, std::memory_order_relaxed);
}

void RecordShaderProgramCreated() {
    sHitchShaderProgramCount.fetch_add(1, std::memory_order_relaxed);
}

// renderMicroseconds covers gfx start..end frame only; scene loads run in game logic before the
// render starts, so the hitch test uses the full interval since the previous end of frame.
void ReportHitchFrame(uint32_t renderMicroseconds, int32_t scene) {
    static OSTime sLastEndOfFrame = 0;
    const OSTime now = OSGetSystemTime();
    const uint32_t microseconds =
        sLastEndOfFrame != 0 ? static_cast<uint32_t>(OSTicksToMicroseconds(now - sLastEndOfFrame)) : 0;
    sLastEndOfFrame = now;
    const O2rArchive::HitchStats o2r = O2rArchive::GetHitchStats();
    const uint32_t resourceMilliseconds =
        static_cast<uint32_t>(sHitchResourceFactoryMicroseconds.exchange(0, std::memory_order_relaxed) / 1000);
    const uint32_t textureMilliseconds =
        static_cast<uint32_t>(sHitchTextureUploadMicroseconds.exchange(0, std::memory_order_relaxed) / 1000);
    const uint32_t textureUploads = sHitchTextureUploadCount.exchange(0, std::memory_order_relaxed);
    const uint32_t shaderPrograms = sHitchShaderProgramCount.exchange(0, std::memory_order_relaxed);

    if (microseconds <= 100000) {
        sHitchBurstLines = 0;
        return;
    }
    if (sHitchBurstLines >= 3) {
        return;
    }
    ++sHitchBurstLines;

    Emit("HITCH: frameMs=%u renderMs=%u scene=0x%08X o2rLoads=%u o2rKB=%u o2rReadMs=%u "
         "topReadMs=%s:%u,%s:%u,%s:%u resourceMs=%u texUploads=%u texMs=%u shaders=%u\n",
         (microseconds + 500) / 1000, (renderMicroseconds + 500) / 1000, static_cast<uint32_t>(scene), o2r.loads,
         static_cast<uint32_t>(o2r.compressedBytes / 1024), static_cast<uint32_t>(o2r.readMicroseconds / 1000),
         o2r.topArchives[0].name[0] ? o2r.topArchives[0].name : "-",
         static_cast<uint32_t>(o2r.topArchives[0].readMicroseconds / 1000),
         o2r.topArchives[1].name[0] ? o2r.topArchives[1].name : "-",
         static_cast<uint32_t>(o2r.topArchives[1].readMicroseconds / 1000),
         o2r.topArchives[2].name[0] ? o2r.topArchives[2].name : "-",
         static_cast<uint32_t>(o2r.topArchives[2].readMicroseconds / 1000),
         resourceMilliseconds, textureUploads, textureMilliseconds, shaderPrograms);
}

static OSThread sThread;
static OSThread* sSampleThread = nullptr;
// 32 KB, statically reserved. The watchdog must survive a stall inside the allocator,
// so nothing here may touch the heap after Start() returns.
static uint8_t sStack[32 * 1024] __attribute__((aligned(16)));
static int sSocket = -1;
static bool sStarted = false;

static const char* PhaseName(uint32_t phase) {
    switch (phase & ~PH_DONE) {
        case PH_NONE:
            return "none";
        case PH_DL_STEP:
            return "dl-step";
        case PH_LOAD_RES:
            return "load-resource";
        case PH_ARCHIVE:
            return "archive-read";
        case PH_TEX_ALLOC:
            return "gx2-tex-alloc";
        case PH_TEX_UPLOAD:
            return "gx2-tex-upload";
        case PH_FLUSH:
            return "flush";
        case PH_ENDFRAME:
            return "end-frame";
        case PH_GX2_DRAW_DONE:
            return "gx2-draw-done";
        case PH_GX2_SLOT_WAIT:
            return "gx2-slot-wait";
        case PH_GX2_SET_PIXEL_TEXTURE:
            return "gx2-set-pixel-texture";
        case PH_GX2_DRAW_TRIANGLES:
            return "gx2-draw-triangles";
        case PH_GX2_GENERATE_SHADER:
            return "gx2-generate-shader";
        case PH_GX2_SET_FETCH_SHADER:
            return "gx2-set-fetch-shader";
        case PH_GX2_SET_VERTEX_SHADER:
            return "gx2-set-vertex-shader";
        case PH_GX2_SET_PIXEL_SHADER:
            return "gx2-set-pixel-shader";
        case PH_TEX_UPLOAD_FN:
            return "gx2-upload-texture-fn";
        case PH_GX2_NEW_TEXTURE:
            return "gx2-new-texture";
        case PH_GX2_SELECT_TEXTURE:
            return "gx2-select-texture";
        case PH_GX2_SET_SAMPLER:
            return "gx2-set-sampler";
        case PH_GUI_TEXTURE:
            return "gui-texture-load";
        case PH_IMGUI_FONT_TEX:
            return "imgui-fonts-texture";
        case PH_IMGUI_DEVICE_OBJECTS:
            return "imgui-device-objects";
        case PH_IMGUI_NEW_FRAME:
            return "imgui-new-frame";
        case PH_GAME_INIT:
            return "game-init";
        default:
            return "?";
    }
}

// snprintf into a fixed buffer, then one sendto. No std::string, no fmt, no spdlog,
// no allocation - see the header for why that matters.
void Emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Emit(const char* fmt, ...) {
    // MUST stay a stack local. With gEventStream on, the main thread (Enter/Leave) and the
    // watchdog thread (tick lines) call this concurrently; a shared static buffer would
    // interleave them and the channel would lie - the third time instrumentation would have
    // lied in this hunt. 512 bytes fits the watchdog's 32 KB stack, and stack use keeps
    // Emit allocation-free, which is the one rule this file exists to obey. 384 rather than
    // 512 because Emit is also called from the exception-registration threads and from the
    // exception callbacks, whose stacks are 4 KB, and losing an EXC: line to a blown stack
    // would cost the evidence most worth having. Not smaller than 384: the worst-case
    // `alive` line is 281 bytes (a 127-byte detail plus every numeric field at full width),
    // and truncating it would drop `flips=` off the end - the field the tick exists to
    // report. vsnprintf truncates safely either way, but silently.
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0 || sSocket < 0) {
        return;
    }
    if (n > (int)sizeof(line) - 1) {
        n = (int)sizeof(line) - 1;
    }

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(4405);
    to.sin_addr.s_addr = htonl(kLogHostAddr);
    // The return value was ignored, so a channel that had stopped delivering was
    // indistinguishable from a console that had stopped running.
    if (sendto(sSocket, line, n, 0, (struct sockaddr*)&to, sizeof(to)) < 0) {
        ++gEmitFail;
    } else {
        ++gEmitOk;
    }
}

void HeapMark(const char* label) {
    const struct mallinfo heapInfo = mallinfo();
    Emit("HEAPMARK: %s used=%u\n", label != nullptr ? label : "?", (uint32_t)heapInfo.uordblks);
}

static uint32_t ProfileHash(uint32_t address) {
    return ((address >> 4) ^ (address >> 12) ^ (address >> 20)) & (kProfileTableSize - 1);
}

static void ProfileAdd(ProfileBucket* table, uint32_t address) {
    if (address == 0) {
        return;
    }

    const uint32_t start = ProfileHash(address);
    for (uint32_t probe = 0; probe < kProfileTableSize; ++probe) {
        ProfileBucket* bucket = &table[(start + probe) & (kProfileTableSize - 1)];
        if (bucket->address == address) {
            ++bucket->count;
            return;
        }
        if (bucket->address == 0) {
            bucket->address = address;
            bucket->count = 1;
            return;
        }
    }
}

static void ProfileSelectTop(const ProfileBucket* table, ProfileTop* top, uint32_t topCount) {
    memset(top, 0, sizeof(ProfileTop) * topCount);

    for (uint32_t bucketIndex = 0; bucketIndex < kProfileTableSize; ++bucketIndex) {
        const ProfileBucket& bucket = table[bucketIndex];
        if (bucket.address == 0 || bucket.count == 0) {
            continue;
        }

        uint32_t insertAt = topCount;
        for (uint32_t topIndex = 0; topIndex < topCount; ++topIndex) {
            if (bucket.count > top[topIndex].count) {
                insertAt = topIndex;
                break;
            }
        }
        if (insertAt == topCount) {
            continue;
        }

        for (uint32_t topIndex = topCount - 1; topIndex > insertAt; --topIndex) {
            top[topIndex] = top[topIndex - 1];
        }
        top[insertAt].address = bucket.address;
        top[insertAt].count = bucket.count;
    }
}

static void EmitProfileTop(const char* kind, const ProfileTop* top, uint32_t topCount) {
    uint32_t first = 0;
    while (first < topCount && top[first].count != 0) {
        char line[384];
        int length = snprintf(line, sizeof(line), "PROF: %s=", kind);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            return;
        }

        uint32_t emitted = 0;
        while (first < topCount && emitted < kProfileEntriesPerLine && top[first].count != 0) {
            const int written = snprintf(line + length, sizeof(line) - (size_t)length, "%s0x%08X:%u",
                                         emitted == 0 ? "" : " ", top[first].address, top[first].count);
            if (written < 0 || (size_t)written >= sizeof(line) - (size_t)length) {
                return;
            }
            length += written;
            ++first;
            ++emitted;
        }
        Emit("%s\n", line);
    }
}

static bool ProfileIsThread(uint32_t address) {
    return address >= 0x10000000u && address < 0x50000000u && (address & 3u) == 0 &&
           *(const volatile uint32_t*)(address + 0x320u) == OS_THREAD_TAG;
}

static void EmitProfileThreads(const ProfileTop* top, uint32_t topCount) {
    for (uint32_t index = 0; index < topCount && top[index].count != 0; ++index) {
        const uint32_t address = top[index].address;
        const char* name = "<not-a-thread>";
        uint32_t entry = 0;
        int32_t priority = 0;
        uint32_t affinity = 0;
        if (ProfileIsThread(address)) {
            const uint32_t nameAddress = *(const volatile uint32_t*)(address + 0x5C0u);
            name = nameAddress != 0 ? (const char*)(uintptr_t)nameAddress : "<unnamed>";
            entry = *(const volatile uint32_t*)(address + 0x39Cu);
            priority = *(const volatile int32_t*)(address + 0x32Cu);
            affinity = *(const volatile uint8_t*)(address + 0x325u);
        }
        Emit("PROF: thr=0x%08X:%u name=%s entry=0x%08X prio=%d aff=0x%X\n", address,
             top[index].count, name, entry, priority, affinity);
    }
}

static void EmitProfileReport() {
    const uint32_t done = sProfileActive;
    sProfileActive = done ^ 1u;
    __sync_synchronize();
    OSSleepTicks(OSMillisecondsToTicks(20)); // let an in-flight callback on the old set finish
    ProfileSelectTop(sPcProfile[done], sTopPcs, kProfileTopPcCount);
    ProfileSelectTop(sLrProfile[done], sTopLrs, kProfileTopLrCount);
    EmitProfileTop("pc", sTopPcs, kProfileTopPcCount);
    EmitProfileTop("lr", sTopLrs, kProfileTopLrCount);
    ProfileSelectTop(sGcProfile[done], sTopPcs, kProfileTopPcCount);
    EmitProfileTop("gc", sTopPcs, kProfileTopPcCount);
    ProfileSelectTop(sAcProfile[done], sTopPcs, kProfileTopPcCount);
    EmitProfileTop("ac", sTopPcs, kProfileTopPcCount);
    ProfileSelectTop(sBkProfile[done], sTopPcs, kProfileTopPcCount);
    EmitProfileTop("bk", sTopPcs, kProfileTopPcCount);
    ProfileSelectTop(sThProfile[done], sTopPcs, kProfileTopPcCount);
    EmitProfileThreads(sTopPcs, kProfileTopPcCount);
    // Every sampled PC, not just the top 24: the host sums them per function, so the long tail
    // (most of the Fast3D interpreter's time) is visible too. ~1000 samples -> ~60 short lines.
    {
        char line[256];
        int length = 0;
        for (uint32_t index = 0; index < kProfileTableSize; ++index) {
            const ProfileBucket& bucket = sPcProfile[done][index];
            if (bucket.address == 0 || bucket.count == 0) {
                continue;
            }
            if (length == 0) {
                length = snprintf(line, sizeof(line), "PROF: all=");
            }
            length += snprintf(line + length, sizeof(line) - (size_t)length, " %X:%u", bucket.address, bucket.count);
            if (length > 200) {
                Emit("%s\n", line);
                length = 0;
            }
        }
        if (length > 0) {
            Emit("%s\n", line);
        }
    }
    Emit("PROF: samples=%u\n", sProfileCount[done]);

    memset(sPcProfile[done], 0, sizeof(sPcProfile[done]));
    memset(sLrProfile[done], 0, sizeof(sLrProfile[done]));
    memset(sGcProfile[done], 0, sizeof(sGcProfile[done]));
    memset(sAcProfile[done], 0, sizeof(sAcProfile[done]));
    memset(sBkProfile[done], 0, sizeof(sBkProfile[done]));
    memset(sThProfile[done], 0, sizeof(sThProfile[done]));
    sProfileCount[done] = 0;
}

// Periodic alarm on the game thread's core. Alarm callbacks run in interrupt context and are
// handed the INTERRUPTED context, so srr0/lr are where the game really was. (OSSuspendThread
// from another core only ever showed the scheduler: every sample landed on one coreinit PC.)
// No locks, no allocation, no Emit in here.
static uint32_t sGameTextStart = 0;
static uint32_t sGameTextEnd = 0;
struct ProfileAddressRange {
    uint32_t start;
    uint32_t end;
};
static ProfileAddressRange sAllocatorRanges[7];
static uint32_t sAllocatorRangeCount = 0;

static void ProfileAddAllocatorRange(uintptr_t address, uint32_t size) {
    if (sAllocatorRangeCount < sizeof(sAllocatorRanges) / sizeof(sAllocatorRanges[0])) {
        sAllocatorRanges[sAllocatorRangeCount].start = (uint32_t)address;
        sAllocatorRanges[sAllocatorRangeCount].end = (uint32_t)address + size;
        ++sAllocatorRangeCount;
    }
}

// The --wrap entry points every allocation goes through, named by asm label (their declarations
// overload in this TU, so &__wrap_malloc has no single type). Do NOT add newlib's _malloc_r & co:
// referencing them pulled wut's own malloc object in instead of newlib's and soh923p3 never booted.

extern "C" const char kProfSym__wrap_malloc[] __asm__("__wrap_malloc");
extern "C" const char kProfSym__wrap_free[] __asm__("__wrap_free");
extern "C" const char kProfSym__wrap_memalign[] __asm__("__wrap_memalign");
extern "C" const char kProfSym__wrap_realloc[] __asm__("__wrap_realloc");
extern "C" const char kProfSym__wrap_calloc[] __asm__("__wrap_calloc");
extern "C" const char kProfSym__wrap__Znwj[] __asm__("__wrap__Znwj");
// libstdc++'s operator new sits between __wrap__Znwj and __wrap_malloc. __wrap__Znwj above already
// calls __real__Znwj, so this reference resolves exactly as before and pulls nothing new in.
extern "C" const char kProfSym__real__Znwj[] __asm__("__real__Znwj");

static void ProfileInitAllocatorRanges() {
    // Sizes from nm -S of soh923p2 (0xF8-0x198), rounded up so a range never covers a neighbour.
    sAllocatorRangeCount = 0;
    ProfileAddAllocatorRange((uintptr_t)kProfSym__wrap_malloc, 0x100);
    ProfileAddAllocatorRange((uintptr_t)kProfSym__wrap_free, 0x110);
    ProfileAddAllocatorRange((uintptr_t)kProfSym__wrap_memalign, 0x110);
    ProfileAddAllocatorRange((uintptr_t)kProfSym__wrap_realloc, 0x120);
    ProfileAddAllocatorRange((uintptr_t)kProfSym__wrap_calloc, 0x1A0);
    ProfileAddAllocatorRange((uintptr_t)kProfSym__wrap__Znwj, 0x100);
    ProfileAddAllocatorRange((uintptr_t)kProfSym__real__Znwj, 0x80);
}

static inline bool ProfileIsAllocator(uint32_t address) {
    for (uint32_t index = 0; index < sAllocatorRangeCount; ++index) {
        if (address >= sAllocatorRanges[index].start && address < sAllocatorRanges[index].end) {
            return true;
        }
    }
    return false;
}

static inline bool ProfileIsGameText(uint32_t address) {
    return address >= sGameTextStart && address < sGameTextEnd;
}

// Stack bounds of the thread being walked. The walkers run inside an alarm interrupt, where a fault is a
// console wedge, so every frame pointer must lie inside the thread's own stack before it is dereferenced.
// A range check against 0x50000000 alone was not enough: a stale frame slot holding ASCII ("O2R:",
// 0x4F32523A) passed it and the read faulted (DSI in ProfileAlarmCallback, SoH develop, 2026-09-29).
struct ProfileStack {
    uint32_t lo; // OSThread::stackEnd (lowest address)
    uint32_t hi; // OSThread::stackStart (one past the highest address)
};

static ProfileStack ProfileStackOf(const OSThread* thread) {
    if (thread == nullptr) {
        return { 0, 0 };
    }
    const uint32_t lo = (uint32_t)(uintptr_t)thread->stackEnd;
    const uint32_t hi = (uint32_t)(uintptr_t)thread->stackStart;
    if (lo < 0x10000000u || hi > 0x50000000u || lo >= hi) {
        return { 0, 0 };
    }
    return { lo, hi };
}

static bool ProfileFrameOk(uint32_t sp, const ProfileStack& stack) {
    return stack.hi != 0 && (sp & 7u) == 0 && sp >= stack.lo && sp + 8u <= stack.hi;
}

static uint32_t ProfileFirstGameReturn(uint32_t lr, uint32_t sp, const ProfileStack& stack) {
    if (ProfileIsGameText(lr)) {
        return lr;
    }
    for (int depth = 0; depth < 24; ++depth) {
        if (!ProfileFrameOk(sp, stack)) {
            return 0;
        }
        const uint32_t next = *(const volatile uint32_t*)sp;
        if (next <= sp || !ProfileFrameOk(next, stack)) {
            return 0;
        }
        const uint32_t savedLr = *(const volatile uint32_t*)(next + 4);
        if (ProfileIsGameText(savedLr)) {
            return savedLr;
        }
        sp = next;
    }
    return 0;
}

// Allocator caller: the first game return above a __wrap_* frame. Everything below the wrapper
// (newlib, heap locks) is allocator internals, so no other allocator symbol is needed.
// Returns 0 when the sample is not inside an allocation at all.
static uint32_t ProfileAllocatorCaller(uint32_t pc, uint32_t lr, uint32_t sp, const ProfileStack& stack) {
    bool aboveWrapper = ProfileIsAllocator(pc);
    if (ProfileIsGameText(lr)) {
        if (ProfileIsAllocator(lr)) {
            aboveWrapper = true;
        } else if (aboveWrapper) {
            return lr;
        }
    }
    for (int depth = 0; depth < 24; ++depth) {
        if (!ProfileFrameOk(sp, stack)) {
            break;
        }
        const uint32_t next = *(const volatile uint32_t*)sp;
        if (next <= sp || !ProfileFrameOk(next, stack)) {
            break;
        }
        const uint32_t savedLr = *(const volatile uint32_t*)(next + 4);
        if (ProfileIsGameText(savedLr)) {
            if (ProfileIsAllocator(savedLr)) {
                aboveWrapper = true;
            } else if (aboveWrapper) {
                return savedLr;
            }
        }
        sp = next;
    }
    return aboveWrapper ? 0xFFFFFFF0u : 0;
}

static void ProfileAlarmCallback(OSAlarm* alarm, OSContext* context) {
    (void)alarm;
    const uint32_t set = sProfileActive;
    uint32_t thread = 0xFFFFFFF1u;
    if (context != nullptr) {
        const uint32_t address = (uint32_t)(uintptr_t)context;
        if (address >= 0x10000000u && address < 0x50000000u && (address & 3u) == 0 &&
            *(const volatile uint32_t*)(address + 0x320u) == OS_THREAD_TAG) {
            thread = address;
        }
    }
    ProfileAdd(sThProfile[set], thread);
    if (context == nullptr) {
        ++sProfileCount[set];
        return;
    }
    ProfileAdd(sPcProfile[set], context->srr0);
    ProfileAdd(sLrProfile[set], context->lr & ~0xFu);
    const uint32_t pc = context->srr0;
    // The OSContext is the first member of its OSThread, so a validated thread gives the stack bounds;
    // with no identifiable thread the walkers see an empty stack and record "unknown" instead of reading.
    const ProfileStack stack =
        thread != 0xFFFFFFF1u ? ProfileStackOf((const OSThread*)(uintptr_t)thread) : ProfileStack{ 0, 0 };
    const uint32_t allocatorCaller = ProfileAllocatorCaller(pc, context->lr, context->gpr[1], stack);
    if (allocatorCaller != 0) {
        ProfileAdd(sAcProfile[set], allocatorCaller & ~0xFu);
    }
    if (!ProfileIsGameText(pc)) {
        const uint32_t caller = ProfileFirstGameReturn(context->lr, context->gpr[1], stack);
        ProfileAdd(sGcProfile[set], caller != 0 ? (caller & ~0xFu) : 0xFFFFFFF0u);
    }
    if (sSampleThread != nullptr && context != &sSampleThread->context) {
        const uint32_t blocked =
            ProfileFirstGameReturn(sSampleThread->context.lr, sSampleThread->context.gpr[1],
                                   ProfileStackOf(sSampleThread));
        ProfileAdd(sBkProfile[set], blocked != 0 ? (blocked & ~0xFu) : 0xFFFFFFF0u);
    }
    ++sProfileCount[set];
}

// Real addresses of likely-hot system exports, so host-side symbolisation can name the
// <system 0x010xxxxx> samples by the nearest preceding landmark.
static void EmitSystemLandmarks() {
    static const char* const kCoreinit[] = { "OSLockMutex", "OSUnlockMutex", "OSWaitCond", "OSSignalCond",
        "OSWaitEvent", "OSSleepTicks", "OSYieldThread", "OSFastMutex_Lock", "OSUninterruptibleSpinLock_Acquire",
        "DCFlushRange", "DCStoreRange", "DCInvalidateRange", "OSBlockMove", "OSBlockSet", "memcpy", "memset",
        "OSDisableInterrupts", "OSRestoreInterrupts", "OSSuspendThread", "OSResumeThread", "OSSleepThread",
        "OSWakeupThread", "__OSLockScheduler",
        "OSGetTime", "OSGetSystemTime", "MEMAllocFromExpHeapEx", "MEMFreeToExpHeap", "OSCompareAndSwapAtomic",
        "OSTestThreadCancel", "OSWaitAlarm", nullptr };
    static const char* const kGx2[] = { "GX2DrawEx", "GX2DrawIndexedEx", "GX2SetAttribBuffer", "GX2SetFetchShader",
        "GX2SetVertexShader", "GX2SetPixelShader", "GX2SetPixelTexture", "GX2SetPixelSampler",
        "GX2SetVertexUniformReg", "GX2SetPixelUniformReg", "GX2SetBlendControl", "GX2SetColorControl",
        "GX2SetDepthStencilControl", "GX2SetAlphaTest", "GX2SetScissor", "GX2SetViewport", "GX2Invalidate",
        "GX2Flush", "GX2DrawDone", "GX2WaitTimeStamp", "GX2WaitForVsync", "GX2CopySurfaceEx",
        "GX2SetPolygonOffset", "GX2SetPolygonControl", "GX2SetCullOnlyControl", nullptr };
    struct Lib { const char* rpl; const char* const* names; };
    const Lib libs[] = { { "coreinit.rpl", kCoreinit }, { "gx2.rpl", kGx2 } };
    for (const Lib& lib : libs) {
        OSDynLoad_Module module = nullptr;
        if (OSDynLoad_Acquire(lib.rpl, &module) != OS_DYNLOAD_OK) {
            Emit("PROFMAP: acquire %s failed\n", lib.rpl);
            continue;
        }
        for (const char* const* name = lib.names; *name != nullptr; ++name) {
            void* address = nullptr;
            if (OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, *name, &address) == OS_DYNLOAD_OK) {
                Emit("PROFMAP: sym %s:%s=0x%08X\n", lib.rpl, *name, (uint32_t)address);
            }
        }
        OSDynLoad_Release(module);
    }
    OSDynLoad_NotifyData infos[48];
    const int32_t rplCount = OSDynLoad_GetNumberOfRPLs();
    if (rplCount > 0 && OSDynLoad_GetRPLInfo(0, rplCount > 48 ? 48 : (uint32_t)rplCount, infos)) {
        for (int32_t i = 0; i < rplCount && i < 48; ++i) {
            Emit("PROFMAP: rpl %s text=0x%08X+0x%X\n", infos[i].name ? infos[i].name : "?", infos[i].textAddr,
                 infos[i].textSize);
        }
    } else {
        Emit("PROFMAP: rpl list unavailable (count=%d)\n", (int)rplCount);
    }
}

// Must run before the title tears down: a periodic alarm left armed would fire into a
// process that is being unmapped.
void StopProfiler() {
    OSCancelAlarm(&sProfileAlarm);
}

// Called from the watchdog loop every kProfileSampleIntervalMs; reports every ~10 s.
static void ProfileSample() {
    static uint32_t iterations = 0;
    if (++iterations >= kProfileSamplesPerReport) {
        iterations = 0;
        EmitProfileReport();
    }
}

void TraceEvent(uint32_t phase, const char* event) {
    if (gTraceState != TRACE_ACTIVE && gEventStream == 0) {
        return;
    }
    // Resource loading is the flood: two events per asset, hundreds of assets, and it is what
    // pushed a capture to 397 lines/second - past anything this channel has been measured to
    // survive. These phases still update the breadcrumb, so the 500 ms tick reports which asset
    // the game is on; they just do not each get their own datagram. Everything that localises a
    // hang - game-init steps, GX2, ImGui - keeps streaming.
    if (gTraceState != TRACE_ACTIVE && (phase == PH_LOAD_RES || phase == PH_ARCHIVE)) {
        return;
    }
    if (gTraceState == TRACE_ACTIVE && phase == PH_TEX_UPLOAD && strcmp(event, "enter") == 0 &&
        gTraceStepState == TRACE_STEPS_WAITING && gDetailFmt == nullptr && strstr(gDetail, "font_letter") != nullptr) {
        gTraceStepState = TRACE_STEPS_ACTIVE;
        gTraceStepsRemaining = WDOG_TRACE_STEP_COUNT;
    }
    // gSeq on every line: the receiver can now tell "the game stopped" from "the datagrams
    // stopped arriving" by whether the next line it sees skipped sequence numbers.
    char detailBuf[128];
    Emit("WDOG: trace seq=%u frame=%u phase=%s event=%s detail=%s\n", gSeq, gFrame, PhaseName(phase), event,
         DetailText(detailBuf, sizeof(detailBuf)));
}

void TraceStep(uint32_t opcode, const void* cmd, uint32_t steps) {
    if (gTraceState != TRACE_ACTIVE || gTraceStepState != TRACE_STEPS_ACTIVE || gTraceStepsRemaining == 0) {
        return;
    }
    Emit("WDOG: trace frame=%u phase=dl-step event=step steps=%u opcode=0x%02X cmd=0x%08X\n", gFrame, steps,
         opcode, (uint32_t)(uintptr_t)cmd);
    --gTraceStepsRemaining;
    if (gTraceStepsRemaining == 0) {
        gTraceStepState = TRACE_STEPS_DONE;
    }
}

void ArmTraceAfterTrackLoad() {
    gTraceFramesRemaining = 0;
    gTraceStepsRemaining = 0;
    gTraceStepState = TRACE_STEPS_WAITING;
    gTraceState = TRACE_ARMED;
}

static int Main(int, const char**) {
    Emit("WDOG: online core=%d - if you never see this line the socket is the problem, not the game\n",
         (int)OSGetCoreId());

    uint32_t lastSeq = 0xFFFFFFFFu;
    uint32_t stalledTicks = 0;
    uint32_t tick = 0;
    uint32_t samplesSinceWatchdogTick = 0;

    for (;;) {
        ProfileSample();
        ++samplesSinceWatchdogTick;
        if (samplesSinceWatchdogTick < kProfileSamplesPerWatchdogTick) {
            OSSleepTicks(OSMillisecondsToTicks(kProfileSampleIntervalMs));
            continue;
        }
        samplesSinceWatchdogTick = 0;

        char detailBuf[128];
        const uint32_t seq = gSeq;
        const uint32_t phase = gPhase;
        const uint32_t frame = gFrame;
        const uint32_t steps = gSteps;
        const uint32_t opcode = gOpcode;
        const uint32_t cmd = gCmd;

        if (seq == lastSeq) {
            ++stalledTicks;
            // No self-rescue here. The SYSLaunchMenu() attempt that used to fire after
            // WDOG_STALL_RESCUE_MS could not tell a hang from the HOME menu or a slow exit (both stop
            // flips), and calling it while the title was exiting DSI'd in __OSClearCopyData
            // (<- SYSCheckSystemApplicationExists) and wedged the console (2026-09-27 16:36, and the
            // HOME -> Close freeze at 19:03). Report only.
            // The main thread has not moved. Whatever `phase` says is where it stopped;
            // a DONE flag means it stopped just after that call returned, not inside it.
            Emit("WDOG: STALLED %ums phase=%s%s frame=%u steps=%u opcode=0x%02X cmd=0x%08X detail=%s flips=%u\n",
                 stalledTicks * WDOG_TICK_INTERVAL_MS, PhaseName(phase), (phase & PH_DONE) ? "(returned)" : "(in progress)",
                 frame, steps, opcode, cmd, DetailText(detailBuf, sizeof(detailBuf)), gFlipCount);
        } else {
            if (stalledTicks != 0) {
                Emit("WDOG: recovered after %ums\n", stalledTicks * WDOG_TICK_INTERVAL_MS);
            }
            stalledTicks = 0;
            lastSeq = seq;
            Emit("WDOG: alive seq=%u phase=%s%s frame=%u steps=%u opcode=0x%02X cmd=0x%08X detail=%s flips=%u\n",
                 seq, PhaseName(phase), (phase & PH_DONE) ? "(returned)" : "(in progress)", frame, steps, opcode,
                 cmd, DetailText(detailBuf, sizeof(detailBuf)), gFlipCount);
        }

        ++tick;

        // MEM2 pressure, every 20th tick (~10 s) and on its own line. Deliberately NOT folded
        // into the alive/STALLED lines: Emit's buffer is 512 bytes and a long detail= path
        // would push these fields off the end of exactly the line we most need intact.
        if ((tick % 20u) == 0u) {
            const MEMHeapHandle mem2 = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2);
            // Cross-check: heapFree is a base-heap query whose heap type is assumed, so it is
            // unverified on first use. texBytes/texCount/lastPtr are our own counters and
            // cannot silently lie. If the two disagree, believe the counters.
            // mem2HeapFree read 0 on every beat of round 3, so the "MEM2 base heap is an
            // ExpHeap" assumption is WRONG and the field is dropped rather than left lying.
            // texCount/texBytes/lastTexPtr are our own counters and are kept.
            (void)mem2;
            const uint32_t timingFrames = __sync_lock_test_and_set(&gFrameTimingCount, 0);
            const uint32_t timingGpuUs = __sync_lock_test_and_set(&gFrameTimingGpuUs, 0);
            const uint32_t timingCpuUs = __sync_lock_test_and_set(&gFrameTimingCpuUs, 0);
            const uint32_t timingWaitUs = __sync_lock_test_and_set(&gFrameTimingWaitUs, 0);
            const uint32_t slotWaits = __sync_lock_test_and_set(&gGx2SlotWaits, 0);
            const uint32_t gpuAvgUs = timingFrames ? timingGpuUs / timingFrames : 0;
            const uint32_t cpuAvgUs = timingFrames ? timingCpuUs / timingFrames : 0;
            const uint32_t waitAvgUs = timingFrames ? timingWaitUs / timingFrames : 0;
            // newlib heap (what malloc/new actually use): arena = bytes obtained via sbrk,
            // used = bytes in allocated chunks. Growth of "used" is the leak/pressure signal.
            const struct mallinfo heapInfo = mallinfo();
            uint32_t o2rLoads = 0;
            uint64_t o2rCompressedBytes = 0;
            uint64_t o2rMicroseconds = 0;
            O2rArchive::GetStats(o2rLoads, o2rCompressedBytes, o2rMicroseconds);
            // Two lines: with the live cache counters one line exceeded Emit's 512-byte buffer
            // and would have truncated heapUsed, the field that matters most.
            Emit("WDOG: mem texCount=%u texBytes=%u texLiveCount=%u texLiveBytes=%u lastTexPtr=0x%08X "
                 "audioSeq=%u audioPhase=%u flips=%u emitOk=%u emitFail=%u otrHit=%u otrMiss=%u rmLookup=%u "
                 "drawHwm=%u frames=%u gpuUs=%u cpuUs=%u waitUs=%u slotWaits=%u heapArena=%u heapUsed=%u\n",
                 gTexCount, gTexBytes, gTexLiveCount, gTexLiveBytes, gLastTexPtr, gAudioSeq, gAudioPhase,
                 gFlipCount, gEmitOk, gEmitFail, gOtrCacheHits, gOtrCacheMisses, gOtrResourceManagerLookups,
                 gDrawBufferHighWaterBytes, timingFrames, gpuAvgUs, cpuAvgUs, waitAvgUs, slotWaits,
                 (uint32_t)heapInfo.arena, (uint32_t)heapInfo.uordblks);
            Emit("WDOG: caches texCache=%u freeTexIds=%u otrCache=%u rawPath=%u rawHash=%u resourceCache=%u "
                 "shaderPool=%u texLiveBytes=%u heapUsed=%u bigFree=%u bigLargest=%u o2rLoads=%u o2rKB=%u o2rMs=%u\n",
                 gTextureCacheSize, gFreeTextureIdsSize, gOtrTextureCacheSize, gRawPointerByPathSize,
                 gRawPointerByHashSize, gResourceCacheSize, gShaderProgramPoolSize, gTexLiveBytes,
                 (uint32_t)heapInfo.uordblks, BigHeapFreeBytes(), BigHeapLargestFree(), o2rLoads,
                 static_cast<uint32_t>(o2rCompressedBytes / 1024), static_cast<uint32_t>(o2rMicroseconds / 1000));
        }

        // The allocation table is sampled independently of the heap-pressure line.  At 500 ms
        // per watchdog tick this is one report about every 60 seconds.
        if ((tick % 120u) == 0u) {
            LeakEmitReport();
        }

        OSSleepTicks(OSMillisecondsToTicks(kProfileSampleIntervalMs));
    }
    return 0;
}

static void TerminateHandler() {
    StopProfiler();
    Emit("CXX: terminate\n");
    // Do NOT fall through to abort(). abort() hard-wedges the PowerPC side and takes
    // ftpiiu and the wiiload server with it, so every uncaught exception has been
    // costing a physical power cycle - the console still answers ping, but nothing
    // that needs CPU does, and it cannot be recovered remotely.
    //
    // _Exit() instead: no destructors (we are already in a terminate handler, the heap
    // and locks cannot be trusted), but the title ends the way the system expects and
    // the Aroma plugins survive. A crash then costs a wiiload, not a reboot.
    _Exit(1);
}

void Start() {
    std::set_terminate(TerminateHandler);

    if (sStarted) {
        return;
    }
    sStarted = true;
    sSampleThread = OSGetCurrentThread();
    // Game .text range for caller attribution: ELF .text starts at 0x02000000 and is ~46 MB;
    // the load bias comes from __init's fixed head-of-.text address, as in the throw logger.
    {
        const uint32_t bias = (uint32_t)(uintptr_t)&__init - 0x02000080u;
        sGameTextStart = bias + 0x02000000u;
        // .text currently ends at ELF 0x04BBE870; 0x04C00000 keeps non-code words found on the
        // stack (a 0x04C32E00 data address was attributed as a "caller") out of the tables.
        sGameTextEnd = bias + 0x04C00000u;
    }
    ProfileInitAllocatorRanges();
    // Set from the game thread so the alarm fires on (and samples) the game thread's core.
    // 9973 us, not a whole ms: a 10 ms period phase-locked with AX's 3 ms tick and put exactly 1/3 of
    // all samples on {SYS AX IST} at one PC (2026-09-25).
    OSCreateAlarm(&sProfileAlarm);
    OSSetPeriodicAlarm(&sProfileAlarm, OSGetTime() + OSMillisecondsToTicks(2000),
                       OSMicrosecondsToTicks(9973), ProfileAlarmCallback);

    // wut wants the socket library brought up before any BSD call. WHBLogUdpInit() has
    // already run by this point and does this for its own socket, but whether that init is
    // process-global is not something to assume - calling it again is harmless.
    socket_lib_init();

    sSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (sSocket >= 0) {
        int on = 1;
        setsockopt(sSocket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }

    // Prove the channel works NOW, while everything is healthy, and say so on the ordinary
    // log. Without this, a socket that was never created and a console that is wedged
    // produce identical evidence: no WDOG lines at all. One test datagram from this thread
    // settles which, before it can possibly matter.
    int testRc = -1;
    if (sSocket >= 0) {
        static const char probe[] = "WDOG: channel test from Start()\n";
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(4405);
        to.sin_addr.s_addr = INADDR_BROADCAST;
        testRc = (int)sendto(sSocket, probe, sizeof(probe) - 1, 0, (struct sockaddr*)&to, sizeof(to));
    }
    // On the RAW channel, not spdlog. The log level is now `warn` on Wii U (the trace/info
    // flood was ~290 UDP datagrams a second and is itself a suspect), so an SPDLOG_INFO here
    // would be swallowed -- and this is the one line MK_FREEZE_2026-09-09.md makes the
    // precondition for reading a capture at all.
    Emit("watchdog: socket=%d channel-test-sendto=%d dest=%u.%u.%u.%u:4405 (if socket is -1 the "
         "watchdog cannot report and its silence means nothing)\n",
         sSocket, testRc, (unsigned int)((kLogHostAddr >> 24) & 0xFF), (unsigned int)((kLogHostAddr >> 16) & 0xFF),
         (unsigned int)((kLogHostAddr >> 8) & 0xFF), (unsigned int)(kLogHostAddr & 0xFF));
    // After the channel test: the watchdog socket must exist before these can be sent.
    Emit("PROFMAP: gameText=0x%08X-0x%08X\n", sGameTextStart, sGameTextEnd);
    EmitSystemLandmarks();

    // Core 2. The game loop runs on core 1, so a stalled main thread cannot hold this off.
    // Priority 5 is above the main thread's 16 (lower number wins on Cafe OS).
    if (!OSCreateThread(&sThread, Main, 0, nullptr, sStack + sizeof(sStack), sizeof(sStack), 5,
                        (OSThreadAttributes)OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        return;
    }
    OSSetThreadName(&sThread, "MK freeze watchdog");
    OSResumeThread(&sThread);
}

#else

static void TerminateHandler() {
    OSFatal("CXX: terminate\n");
    _Exit(1);
}

void Start() {
    std::set_terminate(TerminateHandler);
}

#endif // WIIU_DIAGNOSTICS

} // namespace Watchdog

}; // namespace WiiU
}; // namespace Ship

extern "C" void modelRenderWatchdogReportInvalidDisplayList(uint32_t index, uint32_t size, const char* handler,
                                                            const void* node) {
#if WIIU_DIAGNOSTICS
    static uint32_t emitted = 0;
    static uint64_t lastEmitTick = 0;
    static const uint64_t kTicksPerSecond = 62156250ULL;
    const uint64_t now = OSGetSystemTick();
    // The first 20 go out unthrottled: a 1/sec cap samples roughly one report per
    // second out of hundreds per frame, which hid the real spread of bad indices.
    if (emitted >= 20 && lastEmitTick != 0 && now - lastEmitTick < kTicksPerSecond) {
        return;
    }
    ++emitted;
    lastEmitTick = now;

    // Dump the node as it actually sits in memory. If cmd0 reads 5 (big-endian,
    // i.e. already converted) while the s16 array at +8 reads byte-reversed, then
    // the header and the payload of one node disagree -- which the load-time swap
    // cannot produce, and would mean the renderer is reading a different buffer.
    const uint32_t* words = static_cast<const uint32_t*>(node);
    const int16_t* arr = reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(node) + 8);
    Ship::WiiU::Watchdog::Emit(
        "MODEL: skipped invalid display-list index=%u size=%u handler=%s node=%p cmd0=0x%08X size4=0x%08X "
        "arr=[%d,%d,%d]\n",
        index, size, handler, node, words[0], words[1], (int)arr[0], (int)arr[1], (int)arr[2]);
#else
    (void)index;
    (void)size;
    (void)handler;
    (void)node;
#endif
}

#endif
