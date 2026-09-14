#ifdef __WIIU__
// Early boot probe for the pre-main() hang.
//
// SoH 9.2.3 on this console produces ZERO output: the Aroma plugins log normally on
// the same boot, ftpiiu starves, the console stays pingable. Hoisting Ship::WiiU::Init
// to the first statement of InitOTR changed nothing, which rules out everything from
// main() onward. So whatever stops it runs BEFORE main(), where nothing was listening.
//
// Two jobs, and the second is the one that matters:
//
//   1. Bring UDP logging up as early as this binary can. A previous attempt at this
//      (`soh_log_early`) sat at index [2] of the constructor table - but wut links
//      __do_global_ctors_aux, which walks `for (p = __CTOR_END__-1; *p != -1; p--)`,
//      so table index [2] executed 391st of 393. It was at the wrong END of the array
//      and could never have caught an early hang. Placement here is verified by
//      dumping the built RPX with wiiu/rpx_ctors.py and reading off the position -
//      do NOT trust the index without checking it, the ordering is counter-intuitive.
//
//   2. Sample the stuck thread's program counter. A heartbeat only proves the other
//      core is alive; it cannot say WHERE the hang is. Cafe OS saves a suspended
//      thread's OSContext into its OSThread, so suspending core 1's thread and
//      reading context.srr0 gives a real PC. Resolve it with:
//          powerpc-eabi-addr2line -e soh.elf <srr0 - 0x0A000000>
//      (the 0x0A000000 bias is the one the Banjo DSI work established).
//
// This is diagnostic scaffolding. It must not outlive the investigation.

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/core.h>
#include <whb/log.h>
#include <whb/log_udp.h>

namespace {

OSThread sProbeThread;
// A ctor must not rely on any other ctor having run, so the stack is a plain array,
// not a std::vector or anything with an initializer of its own.
alignas(16) unsigned char sProbeStack[8 * 1024];
bool sProbeStarted = false;

int ProbeMain(int, const char**) {
    WHBLogPrintf("EARLYPROBE: sampler online core=%d", (int)OSGetCoreId());

    // Core 1 runs main() on Cafe OS; that is the thread we expect to be stuck.
    OSThread* target = OSGetDefaultThread(1);
    if (target == nullptr) {
        WHBLogPrintf("EARLYPROBE: no core-1 default thread - cannot sample");
        return 0;
    }

    for (unsigned tick = 0;; ++tick) {
        OSSleepTicks(OSMillisecondsToTicks(1000));

        // Suspending flushes the thread's registers into thread->context. Without the
        // suspend the context holds whatever was there at its last deschedule, which
        // for a spinning thread is stale and misleading.
        OSSuspendThread(target);
        const uint32_t srr0 = target->context.srr0;
        const uint32_t lr = target->context.lr;
        const uint32_t gpr1 = target->context.gpr[1];
        OSContinueThread(target);

        WHBLogPrintf("EARLYPROBE: t=%us srr0=0x%08X lr=0x%08X sp=0x%08X", tick + 1, srr0, lr, gpr1);
    }
    return 0;
}

// Runs as a global constructor. Its POSITION is what makes it useful, and position is
// decided by link order plus wut.ld's .ctors bucketing - not by anything written here.
// Verify it with rpx_ctors.py against the built RPX before drawing conclusions from a run.
__attribute__((constructor)) void WiiUEarlyProbeStart() {
    if (sProbeStarted) {
        return;
    }
    sProbeStarted = true;

    WHBLogUdpInit();
    WHBLogPrint("EARLYPROBE: alive - global constructors are executing");

    if (OSCreateThread(&sProbeThread, ProbeMain, 0, nullptr, sProbeStack + sizeof(sProbeStack),
                       sizeof(sProbeStack), 20, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        OSSetThreadName(&sProbeThread, "EarlyProbe");
        OSResumeThread(&sProbeThread);
        WHBLogPrint("EARLYPROBE: sampler thread resumed on core 2");
    } else {
        WHBLogPrint("EARLYPROBE: OSCreateThread FAILED");
    }
}

} // namespace
#endif // __WIIU__
