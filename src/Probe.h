// Probe.h - MagickaPoolFix
//
// Diagnostic tooling kept permanently in the plugin, not a temporary scaffold.
// It exists so the memory offsets can be re-discovered after a game update
// without a disassembler and without a new build.
//
// Two modes:
//
//   Hex dump      - the classic view, 16 bytes per line with the four dwords also
//                   shown as floats. Useful to eyeball structure boundaries.
//
//   Float watch   - the actually useful one. It snapshots every 4-byte aligned
//                   float in a range and, on the next snapshot, reports only the
//                   offsets that changed, with old and new values. Diffing two hex
//                   dumps by eye is error-prone; having the plugin name the offset
//                   that moved by exactly the cost of the spell you just cast is
//                   not.

#pragma once

#include "Log.h"
#include "PCH.h"
#include "PeUtil.h"

#include <cmath>
#include <vector>

namespace mpf {

// A float is "plausible" as a game value when it is finite and not absurdly
// large. Filtering on this keeps the watch output readable: raw pointers and
// integer fields reinterpreted as floats are usually denormal or enormous.
inline bool PlausibleGameFloat(float f) {
    if (!std::isfinite(f)) { return false; }
    const float a = std::fabs(f);
    return a == 0.0f || (a > 1.0e-4f && a < 1.0e7f);
}

// A dump of a 1900-byte object is mostly zeros, and the few offsets that carry a
// value are what matters. This condensed view lists them, so the interesting
// content of a dump fits on one screen instead of 120 hex lines.
// How many aligned offsets in the window hold a non-zero value that looks like a
// game quantity rather than a pointer fragment. This is the cheapest signal we have
// for "the object has been filled in", and unlike the deficit it does not read as
// zero simply because the player happens to be at full Magicka.
inline int CountPlausibleFloats(std::uintptr_t objBase, int from, int to) {
    const int         base = from & ~3;
    const std::size_t n    = static_cast<std::size_t>((to - base) / 4);
    if (n == 0) { return 0; }
    std::vector<float> v(n);
    if (!ReadMem(objBase + static_cast<unsigned>(base), v.data(), n * 4)) { return 0; }
    int count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (v[i] != 0.0f && PlausibleGameFloat(v[i])) { ++count; }
    }
    return count;
}

inline void LogPlausibleFloats(std::uintptr_t objBase, int from, int to,
                               const char* label) {
    const int    base  = from & ~3;
    const std::size_t n = static_cast<std::size_t>((to - base) / 4);
    if (n == 0) { return; }
    std::vector<float> v(n);
    if (!ReadMem(objBase + static_cast<unsigned>(base), v.data(), n * 4)) {
        MPF_INFO("FLOATS %s: object not readable", label);
        return;
    }
    int shown = 0;
    MPF_INFO("FLOATS %s: aligned floats in 0x%X..0x%X that are non-zero and look "
             "like a game value", label, base, to);
    for (std::size_t i = 0; i < n; ++i) {
        if (v[i] == 0.0f || !PlausibleGameFloat(v[i])) { continue; }
        MPF_INFO("  +0x%04X  %14.4f", base + static_cast<int>(i) * 4,
                 static_cast<double>(v[i]));
        ++shown;
    }
    MPF_INFO("FLOATS %s: %d of %zu offsets carry a plausible non-zero value",
             label, shown, n);
}

inline void HexDump(std::uintptr_t objBase, int from, int to, const char* label) {
    MPF_INFO("HEXDUMP %s object=0x%llX range=0x%X..0x%X", label,
             static_cast<unsigned long long>(objBase), from, to);
    for (int off = from; off < to; off += 16) {
        std::uint8_t bytes[16] = {};
        if (!ReadMem(objBase + static_cast<unsigned>(off), bytes, 16)) {
            MPF_INFO("  +0x%04X  <not readable, stopping>", off);
            return;
        }

        char hex[16 * 3 + 1] = {};
        for (int i = 0; i < 16; ++i) {
            std::snprintf(hex + i * 3, 4, "%02X ", bytes[i]);
        }

        float f[4];
        std::memcpy(f, bytes, 16);
        MPF_INFO("  +0x%04X  %s | %14.4f %14.4f %14.4f %14.4f", off, hex,
                 static_cast<double>(f[0]), static_cast<double>(f[1]),
                 static_cast<double>(f[2]), static_cast<double>(f[3]));
    }
}

// Tracks every aligned float in a byte range and reports what moved.
class FloatWatch {
public:
    void Configure(int from, int to) {
        from_ = from & ~3;
        to_   = to;
        values_.assign(static_cast<std::size_t>((to_ - from_) / 4), 0.0f);
        primed_ = false;
    }

    bool Configured() const { return !values_.empty(); }

    // Returns the number of changed offsets, logging each one. When the watch has
    // not been primed yet it only records the baseline and reports nothing, so the
    // first call never produces a wall of spurious changes.
    int Sample(std::uintptr_t objBase, const char* reason, Lvl level) {
        if (values_.empty()) { return 0; }
        const std::size_t bytes = values_.size() * 4;

        std::vector<float> now(values_.size());
        if (!ReadMem(objBase + static_cast<unsigned>(from_), now.data(), bytes)) {
            return 0;
        }

        if (!primed_) {
            values_ = std::move(now);
            primed_ = true;
            MPF_DBG("FLOATWATCH primed: %zu floats in 0x%X..0x%X (%s)",
                     values_.size(), from_, to_, reason);
            return 0;
        }

        int changed = 0;
        for (std::size_t i = 0; i < values_.size(); ++i) {
            const float a = values_[i];
            const float b = now[i];
            if (a == b) { continue; }
            // Skip pairs where neither side looks like a game value: those are
            // pointers or counters, not something worth reporting here.
            if (!PlausibleGameFloat(a) && !PlausibleGameFloat(b)) {
                values_[i] = b;
                continue;
            }
            const int off = from_ + static_cast<int>(i) * 4;
            Log().Write(level, "FLOATWATCH +0x%04X  %14.4f -> %14.4f  (delta %+.4f)  [%s]",
                        off, static_cast<double>(a), static_cast<double>(b),
                        static_cast<double>(b - a), reason);
            values_[i] = b;
            ++changed;
        }
        return changed;
    }

    // A manual dump needs its own baseline. The polling Sample() above runs every
    // iPollIntervalMs and consumes each delta as it reports it, so by the time a
    // key press is handled in the same iteration there is nothing left to show:
    // the manual float watch in 0.1.x and 0.2.x was structurally guaranteed to
    // print nothing at the default log level, which defeated the whole point of
    // the key. This keeps a second, independent snapshot that is only refreshed
    // when a manual dump happens, so a dump answers the question the procedure
    // actually asks -- what moved since the previous press.
    int SampleManual(std::uintptr_t objBase) {
        if (values_.empty()) { return 0; }
        const std::size_t bytes = values_.size() * 4;

        std::vector<float> now(values_.size());
        if (!ReadMem(objBase + static_cast<unsigned>(from_), now.data(), bytes)) {
            MPF_INFO("FLOATWATCH manual: object not readable, nothing to compare");
            return 0;
        }

        if (manual_.size() != now.size()) {
            manual_ = std::move(now);
            MPF_INFO("FLOATWATCH manual baseline set: %zu floats in 0x%X..0x%X. "
                     "Press the dump key again after doing something to see what "
                     "moved in between.", manual_.size(), from_, to_);
            return 0;
        }

        int changed = 0;
        for (std::size_t i = 0; i < manual_.size(); ++i) {
            const float a = manual_[i];
            const float b = now[i];
            if (a == b) { continue; }
            if (!PlausibleGameFloat(a) && !PlausibleGameFloat(b)) { continue; }
            const int off = from_ + static_cast<int>(i) * 4;
            MPF_INFO("FLOATWATCH +0x%04X  %14.4f -> %14.4f  (delta %+.4f)  "
                     "[since previous manual dump]",
                     off, static_cast<double>(a), static_cast<double>(b),
                     static_cast<double>(b - a));
            ++changed;
        }
        if (changed == 0) {
            MPF_INFO("FLOATWATCH manual: nothing plausible changed since the "
                     "previous dump");
        }
        manual_ = std::move(now);
        return changed;
    }

    void Reset() { primed_ = false; manual_.clear(); }

private:
    int                from_ = 0;
    int                to_   = 0;
    bool               primed_ = false;
    std::vector<float> values_;   // continuous poll baseline
    std::vector<float> manual_;   // baseline for key-triggered dumps only
};

// Edge-triggered key state, so holding the key down produces a single dump.
class KeyEdge {
public:
    explicit KeyEdge(int vk) : vk_(vk) {}

    bool Pressed() {
        if (vk_ == 0) { return false; }
        const bool down = (GetAsyncKeyState(vk_) & 0x8000) != 0;
        const bool edge = down && !wasDown_;
        wasDown_ = down;
        return edge;
    }

private:
    int  vk_ = 0;
    bool wasDown_ = false;
};

}  // namespace mpf
