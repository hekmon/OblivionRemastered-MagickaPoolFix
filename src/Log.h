// Log.h - MagickaPoolFix
//
// An ASI plugin has no console, so the log file is the only diagnostic channel.
// Decisions taken (SPEC.md section 10):
//   - single file, next to the .asi;
//   - TRUNCATED at every start, NO rotation, so the game folder never fills up
//     with files nobody will ever clean;
//   - written with WriteFile, NOT with stdio, and never buffered. Two reasons,
//     both learned the hard way (SPEC.md section 9.3):
//       * FILE_SHARE_DELETE lets the player delete the log while the game runs;
//         a C stdio handle forbids that, so a stuck game makes the file
//         undeletable and the player cannot even tell whether it is stale.
//       * a C stdio FILE owns a lock. ExitProcess kills every thread but the
//         caller BEFORE running DLL_PROCESS_DETACH, so the observation thread
//         can die holding that lock; the logger destructor then flushes at
//         detach, blocks on a lock owned by a dead thread, and the process hangs
//         with no window and no way out but the task manager. WriteFile takes no
//         user-mode lock, so that whole class of hang is gone;
//   - 5 levels; the Info level only logs changes, never per-frame state;
//   - hard line cap per session;
//   - bounded self-escalation to Debug detail if calibration fails.

#pragma once

#include "PCH.h"

namespace mpf {

enum class Lvl : std::int32_t {
    Off   = 0,
    Error = 1,
    Info  = 2,
    Debug = 3,
    Trace = 4,
};

class Logger {
public:
    ~Logger() { Shutdown(); }

    // Returns false when the log file could not be opened, which happens when the
    // game is installed in a directory the process may not write to. The caller is
    // expected to retry elsewhere rather than run silently: a plugin that produces
    // no log is indistinguishable from a plugin that never loaded.
    bool Init(const char* path, Lvl level, long maxLines) {
        if (!csReady_) {
            InitializeCriticalSection(&cs_);
            csReady_ = true;
        }
        level_    = level;
        maxLines_ = maxLines;
        started_  = GetTickCount64();
        lines_    = 0;
        capped_   = false;
        if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
        if (level_ == Lvl::Off) { return true; }
        // CREATE_ALWAYS truncates. No rotation, by explicit decision.
        // FILE_SHARE_DELETE is the point: the player can delete or rename this file
        // at any time, including while the game is running or wedged.
        h_ = CreateFileA(path, GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h_ != INVALID_HANDLE_VALUE;
    }

    bool IsOpen() const { return h_ != INVALID_HANDLE_VALUE; }

    // Called from a static destructor, therefore at DLL_PROCESS_DETACH, therefore
    // possibly after ExitProcess has already killed the observation thread at an
    // arbitrary instruction. If that thread died inside Write it still owns cs_, and a
    // critical section owned by a dead thread is never released: taking it here would
    // hang the process on the loader lock. So this never blocks. Abandoning a handle at
    // process exit costs nothing, the kernel closes it; hanging costs the player their
    // session.
    void Shutdown() {
        if (csReady_ && !TryEnterCriticalSection(&cs_)) { return; }
        if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
        if (csReady_) {
            LeaveCriticalSection(&cs_);
            DeleteCriticalSection(&cs_);
            csReady_ = false;
        }
    }

    bool Enabled(Lvl l) const {
        if (h_ == INVALID_HANDLE_VALUE || capped_) { return false; }
        if (escalateRemaining_ > 0 && l <= Lvl::Debug) { return true; }
        return static_cast<std::int32_t>(l) <= static_cast<std::int32_t>(level_);
    }

    Lvl Level() const { return level_; }

    // Temporarily raise the effective level to Debug for a bounded number of
    // lines. This is the only case where the plugin increases its own verbosity,
    // and it says so in the log.
    void Escalate(long lines, const char* reason) {
        if (h_ == INVALID_HANDLE_VALUE || capped_ || lines <= 0) { return; }
        if (static_cast<std::int32_t>(level_) >= static_cast<std::int32_t>(Lvl::Debug)) {
            return;  // already verbose enough
        }
        escalateRemaining_ = lines;
        Write(Lvl::Error, "AUTO-ESCALATION: raising detail to Debug for %ld lines (%s)",
              lines, reason);
    }

    void Write(Lvl l, const char* fmt, ...) {
        if (!Enabled(l)) { return; }

        char    body[1024];
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
        if (n < 0) { return; }

        EnterCriticalSection(&cs_);
        if (h_ != INVALID_HANDLE_VALUE && !capped_) {
            const unsigned long long ms = GetTickCount64() - started_;
            SYSTEMTIME st;
            GetLocalTime(&st);
            char line[1280];
            int  m = std::snprintf(line, sizeof(line),
                                   "[%02u:%02u:%02u.%03u][%9llu ms][%s] %s\r\n",
                                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ms,
                                   Tag(l), body);
            Emit(line, m);

            ++lines_;
            if (escalateRemaining_ > 0) { --escalateRemaining_; }
            if (maxLines_ > 0 && lines_ >= maxLines_) {
                capped_ = true;
                m = std::snprintf(line, sizeof(line),
                                  "[----] LINE CAP REACHED (%ld lines). Logging stops here "
                                  "to avoid filling the disk. Raise iMaxLines in the .conf "
                                  "if you need more, or lower iLevel.\r\n",
                                  maxLines_);
                Emit(line, m);
            }
        }
        LeaveCriticalSection(&cs_);
    }

    void Separator(const char* title) {
        if (!Enabled(Lvl::Info)) { return; }
        Write(Lvl::Info, "---- %s ----", title);
    }

    long Lines() const { return lines_; }

private:
    // One unbuffered write per line. No CRT, no lock, so nothing here can be left
    // wedged by a thread that gets killed mid-call. Partial writes are looped over
    // because WriteFile is allowed to write less than asked.
    void Emit(const char* p, int n) {
        if (n <= 0) { return; }
        if (n >= 1280) { n = 1279; }              // snprintf truncated; write what we have
        DWORD off = 0;
        while (off < static_cast<DWORD>(n)) {
            DWORD wrote = 0;
            if (!WriteFile(h_, p + off, static_cast<DWORD>(n) - off, &wrote, nullptr) ||
                wrote == 0) {
                break;                            // disk full or handle died; drop the line
            }
            off += wrote;
        }
    }

    static const char* Tag(Lvl l) {
        switch (l) {
        case Lvl::Error: return "ERR ";
        case Lvl::Info:  return "INFO";
        case Lvl::Debug: return "DBG ";
        case Lvl::Trace: return "TRCE";
        default:         return "----";
        }
    }

    HANDLE             h_ = INVALID_HANDLE_VALUE;
    Lvl                level_ = Lvl::Info;
    long               maxLines_ = 0;
    long               lines_ = 0;
    long               escalateRemaining_ = 0;
    bool               capped_ = false;
    bool               csReady_ = false;
    unsigned long long started_ = 0;
    CRITICAL_SECTION   cs_{};
};

// The plugin's single logger instance.
inline Logger& Log() {
    static Logger g;
    return g;
}

#define MPF_ERR(...)  ::mpf::Log().Write(::mpf::Lvl::Error, __VA_ARGS__)
#define MPF_INFO(...) ::mpf::Log().Write(::mpf::Lvl::Info,  __VA_ARGS__)
#define MPF_DBG(...)  ::mpf::Log().Write(::mpf::Lvl::Debug, __VA_ARGS__)
#define MPF_TRC(...)  ::mpf::Log().Write(::mpf::Lvl::Trace, __VA_ARGS__)

}  // namespace mpf
