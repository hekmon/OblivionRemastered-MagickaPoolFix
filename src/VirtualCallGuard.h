// VirtualCallGuard.h -- surviving the first call into game code.
//
// Reading game memory is safe here: every read goes through ReadProcessMemory, so
// an unmapped page returns a failure instead of faulting. CALLING a function at a
// vtable index is a different class of risk entirely, and 0.3.1 demonstrated it in
// the field: the log stopped on the line immediately before the first
// GetActorFloatValue call and never produced another entry, while the game itself
// carried on normally for several minutes. The observation thread was lost without
// a crash dialog and without a dump.
//
// Two failure modes explain that, and they need different handling:
//
//   - the call faults, and something in the process (a mod loader's exception
//     filter) swallows the fault and unwinds or discards our thread;
//   - the call blocks forever, because a game method entered from a foreign thread
//     waits on a lock the game's own thread holds.
//
// A timeout cannot detect the first and an exception handler cannot detect the
// second, so this header addresses both:
//
//   1. The first call is made on a throwaway thread. The observation loop waits for
//      it with a timeout, so a hang costs one thread and nothing else -- the loop
//      keeps sampling memory and keeps logging.
//   2. A sentinel file is written and flushed to disk before the attempt and
//      removed after it returns. If it is still there at the next launch, the
//      previous attempt did not come back, and virtual calls are disabled
//      automatically for that build.
//
// Note deliberately not taken: TerminateThread on a hung thread. If it is stuck
// holding or waiting on a game lock, killing it can leave that lock held forever
// and take the game down. Leaking one blocked thread is the lesser harm.

#pragma once

#include "PCH.h"
#include "Log.h"

namespace mpf {

enum class VCallResult {
    Ok,        // the call returned a value
    TimedOut,  // the call did not come back; the thread is leaked on purpose
};

// ---- sentinel ----------------------------------------------------------------

inline std::string VCallSentinelPath(const std::string& dir) {
    return dir + MPF_PLUGIN_NAME ".vcall";
}

// True when a previous run began a virtual call and never recorded its return.
inline bool VCallSentinelPresent(const std::string& path) {
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Written and flushed before the attempt. FILE_FLAG_WRITE_THROUGH matters: if the
// process dies an instant later the bytes must already be on the disk, otherwise
// the sentinel is exactly as lost as the log line would have been.
inline bool VCallSentinelWrite(const std::string& path, const char* text) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    DWORD written = 0;
    const DWORD len = static_cast<DWORD>(std::strlen(text));
    const BOOL ok = WriteFile(h, text, len, &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok && written == len;
}

inline void VCallSentinelClear(const std::string& path) {
    DeleteFileA(path.c_str());
}

// ---- the guarded call --------------------------------------------------------

using GetAvFn = float (MPF_CALL*)(void* self, std::int32_t av);

struct VCallJob {
    GetAvFn      fn   = nullptr;
    void*        self = nullptr;
    std::int32_t av   = 0;
    float        out  = 0.0f;
    // Set by the worker just before it returns. The observation loop only trusts
    // `out` when this is true, so a timed-out call can never feed calibration even
    // if the worker completes long after the wait gave up.
    volatile LONG done = 0;
};

inline DWORD WINAPI VCallWorker(LPVOID p) {
    auto* job = static_cast<VCallJob*>(p);
    job->out = job->fn(job->self, job->av);
    InterlockedExchange(&job->done, 1);
    return 0;
}

// Runs one call on a throwaway thread. The job must outlive the thread, so callers
// pass storage that is never freed -- a leaked page is cheaper than a use-after-free
// in a thread we cannot stop.
inline VCallResult VCallGuarded(VCallJob* job, unsigned timeoutMs, float& out) {
    HANDLE h = CreateThread(nullptr, 64 * 1024, &VCallWorker, job, 0, nullptr);
    if (h == nullptr) {
        return VCallResult::TimedOut;  // treated as unusable, same conclusion
    }
    const DWORD w = WaitForSingleObject(h, timeoutMs);
    if (w == WAIT_OBJECT_0 && InterlockedCompareExchange(&job->done, 1, 1) == 1) {
        CloseHandle(h);
        out = job->out;
        return VCallResult::Ok;
    }
    // Leak the handle and the thread on purpose: see the note at the top.
    return VCallResult::TimedOut;
}

}  // namespace mpf
