// PeUtil.h - MagickaPoolFix
//
// Minimal PE introspection: module inventory, section table walking, and a cache
// of committed readable memory ranges.
//
// Why the range cache matters: this plugin reads a live game object from its own
// thread. A raw dereference of a stale pointer during a load screen would take
// the game down. MinGW does not support MSVC structured exception handling, so
// __try/__except is not available; instead every read is validated against a
// snapshot of committed readable regions obtained from VirtualQuery, and the
// object's vtable pointer is re-checked before every field access.

#pragma once

#include "PCH.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace mpf {

struct Section {
    char           name[9] = {};
    std::uintptr_t start   = 0;
    std::size_t    size    = 0;
    DWORD          characteristics = 0;

    bool Executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
    bool Writable()   const { return (characteristics & IMAGE_SCN_MEM_WRITE) != 0; }
    bool Contains(std::uintptr_t a) const { return a >= start && a < start + size; }
};

// A loaded module plus its parsed section table.
class ModuleImage {
public:
    bool Load(HMODULE h) {
        if (!h) { return false; }
        handle_ = h;
        base_   = reinterpret_cast<std::uintptr_t>(h);

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { return false; }
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) { return false; }
        if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { return false; }

        imageSize_ = nt->OptionalHeader.SizeOfImage;
        entry_     = base_ + nt->OptionalHeader.AddressOfEntryPoint;
        timestamp_ = nt->FileHeader.TimeDateStamp;

        const auto* sec = IMAGE_FIRST_SECTION(nt);
        const int   n   = nt->FileHeader.NumberOfSections;
        sections_.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i, ++sec) {
            Section s;
            std::memcpy(s.name, sec->Name, 8);
            s.name[8]         = '\0';
            s.start           = base_ + sec->VirtualAddress;
            s.size            = sec->Misc.VirtualSize;
            s.characteristics = sec->Characteristics;
            sections_.push_back(s);
        }

        char path[MAX_PATH] = {};
        GetModuleFileNameA(h, path, MAX_PATH);
        path_ = path;
        const auto slash = path_.find_last_of("\\/");
        name_ = (slash == std::string::npos) ? path_ : path_.substr(slash + 1);

        return !sections_.empty();
    }

    HMODULE            Handle()    const { return handle_; }
    std::uintptr_t     Base()      const { return base_; }
    std::size_t        ImageSize() const { return imageSize_; }
    std::uintptr_t     Entry()     const { return entry_; }
    DWORD              Timestamp() const { return timestamp_; }
    const std::string& Path()      const { return path_; }
    const std::string& Name()      const { return name_; }
    const std::vector<Section>& Sections() const { return sections_; }

    std::uint32_t Rva(std::uintptr_t a) const {
        return static_cast<std::uint32_t>(a - base_);
    }

    const Section* Find(const char* n) const {
        for (const auto& s : sections_) {
            if (std::strncmp(s.name, n, 8) == 0) { return &s; }
        }
        return nullptr;
    }

    bool InImage(std::uintptr_t a) const {
        return a >= base_ && a < base_ + imageSize_;
    }

    bool InExecutable(std::uintptr_t a) const {
        for (const auto& s : sections_) {
            if (s.Executable() && s.Contains(a)) { return true; }
        }
        return false;
    }

private:
    HMODULE              handle_ = nullptr;
    std::uintptr_t       base_ = 0;
    std::size_t          imageSize_ = 0;
    std::uintptr_t       entry_ = 0;
    DWORD                timestamp_ = 0;
    std::string          path_;
    std::string          name_;
    std::vector<Section> sections_;
};

// Snapshot of committed, readable, non-guard memory ranges.
// Refreshed on demand; a stale snapshot only causes a read to be skipped, never
// an invalid dereference, because the ranges are checked before every access and
// the object identity is re-verified afterwards.
class MemRegions {
public:
    void Refresh() {
        ranges_.clear();
        heap_.clear();
        MEMORY_BASIC_INFORMATION mbi{};
        std::uintptr_t addr = 0;
        while (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) ==
               sizeof(mbi)) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto end   = begin + mbi.RegionSize;
            const bool readable =
                mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY)) != 0 &&
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
            if (readable) {
                if (!ranges_.empty() && ranges_.back().second == begin) {
                    ranges_.back().second = end;  // coalesce adjacent ranges
                } else {
                    ranges_.emplace_back(begin, end);
                }
                // Private read-write pages are where game objects live. Keeping a
                // separate list of them lets the instance search skip mapped
                // images and file views entirely, which is both faster and safer.
                if (mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
                    if (!heap_.empty() && heap_.back().second == begin) {
                        heap_.back().second = end;
                    } else {
                        heap_.emplace_back(begin, end);
                    }
                }
            }
            if (end <= addr) { break; }  // guard against a non-advancing walk
            addr = end;
        }
    }

    // IMPORTANT: this answers "was this address readable when the snapshot was
    // taken", which is NOT the same as "is it readable now". The game commits and
    // releases heap pages continuously, so a true answer here is only a hint that
    // an address is worth looking at. It is never sufficient on its own to make a
    // dereference safe -- always go through ReadMem() for the actual access.
    bool Readable(std::uintptr_t a, std::size_t n) const {
        return Contains(ranges_, a, n);
    }

    // Same caveat as Readable(): a hint used to reject obviously-bogus pointers
    // cheaply, before paying for a syscall to try reading them.
    bool InHeap(std::uintptr_t a, std::size_t n) const {
        return Contains(heap_, a, n);
    }

    std::size_t Count()     const { return ranges_.size(); }
    std::size_t HeapCount() const { return heap_.size(); }

    const std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& Heap() const {
        return heap_;
    }

    std::size_t HeapBytes() const {
        std::size_t total = 0;
        for (const auto& r : heap_) { total += r.second - r.first; }
        return total;
    }

private:
    static bool Contains(
        const std::vector<std::pair<std::uintptr_t, std::uintptr_t>>& v,
        std::uintptr_t a, std::size_t n) {
        if (a == 0 || n == 0) { return false; }
        const std::uintptr_t last = a + n - 1;
        if (last < a) { return false; }  // overflow
        // Both vectors are built in ascending order, so binary search is valid.
        auto it = std::upper_bound(
            v.begin(), v.end(), a,
            [](std::uintptr_t x, const std::pair<std::uintptr_t, std::uintptr_t>& r) {
                return x < r.first;
            });
        if (it == v.begin()) { return false; }
        --it;
        return a >= it->first && last < it->second;
    }

    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> ranges_;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> heap_;
};

// ---------------------------------------------------------------------------
// The one and only way this plugin touches game memory.
//
// A plain dereference of a game pointer is unsafe from an observer thread: the
// game is free to decommit or release a page at any moment, and a read of an
// unmapped page raises an access violation. This build is compiled with
// MinGW-w64, which offers no structured exception handling, so such a fault is
// unrecoverable and takes the process down.
//
// ReadProcessMemory on our own process handle solves this at the source. The
// kernel probes the source range and copies through, so an unmapped or
// protection-changed page makes the call return FALSE (usually with
// ERROR_PARTIAL_COPY) instead of faulting in our thread. The cost is one
// syscall per call site, which is irrelevant at the rate this plugin reads.
//
// Everything that reads the game -- pointer scans, the hex dump, the float
// watch, the actor value samples -- goes through here. Nothing dereferences a
// game pointer directly.
inline bool ReadMem(std::uintptr_t addr, void* dst, std::size_t n) {
    if (addr == 0 || dst == nullptr || n == 0) { return false; }
    if (addr + n < addr) { return false; }  // overflow
    SIZE_T got = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(addr),
                             dst, n, &got)) {
        return false;
    }
    return got == n;
}

// The single write primitive of the whole plugin.
//
// WriteProcessMemory on the current process rather than a plain store, for the same
// reason ReadMem uses ReadProcessMemory: the kernel validates the address and returns
// an error instead of raising an access violation. A wrong pointer then produces a log
// line rather than a crash to desktop, which matters because this plugin is loaded into
// somebody's game and a fault here loses their unsaved progress.
//
// The read-back is not paranoia about the API. It catches the case where the page is
// writable but the value does not stick because the engine owns the field and rewrites
// it the same frame; the caller needs to know that so it can stop trying.
inline bool WriteMem(std::uintptr_t addr, const void* src, std::size_t n) {
    if (addr == 0 || src == nullptr || n == 0) { return false; }
    if (addr + n < addr) { return false; }  // overflow
    SIZE_T put = 0;
    if (!::WriteProcessMemory(::GetCurrentProcess(),
                              reinterpret_cast<LPVOID>(addr),
                              src, n, &put)) {
        return false;
    }
    return put == n;
}

template <typename T>
inline bool SafeWrite(std::uintptr_t addr, const T& value) {
    if (!WriteMem(addr, &value, sizeof(T))) { return false; }
    T back{};
    if (!ReadMem(addr, &back, sizeof(T))) { return false; }
    return std::memcmp(&back, &value, sizeof(T)) == 0;
}

// A safe typed read: returns false rather than faulting.
template <typename T>
inline bool SafeRead(std::uintptr_t addr, T& out) {
    return ReadMem(addr, &out, sizeof(T));
}

// Kept for call sites that still want the cheap snapshot pre-filter. The region
// hint only avoids a pointless syscall; ReadMem() is what makes the read safe.
template <typename T>
inline bool SafeRead(const MemRegions& m, std::uintptr_t addr, T& out) {
    if (!m.Readable(addr, sizeof(T))) { return false; }
    return ReadMem(addr, &out, sizeof(T));
}

// Lowercased Windows directory with a trailing separator, used to tell a stock
// system library from a module that merely borrowed its name.
inline std::string WindowsDirLower() {
    char buf[MAX_PATH] = {};
    const UINT n = ::GetSystemWindowsDirectoryA(buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return std::string(); }
    std::string s(buf, n);
    if (!s.empty() && s.back() != '\\') { s.push_back('\\'); }
    for (auto& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

struct ModuleRow {
    std::string    name;
    std::string    path;
    std::uintptr_t base = 0;
    std::size_t    size = 0;
};

inline std::vector<ModuleRow> EnumerateModules() {
    std::vector<ModuleRow> rows;
    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        return rows;
    }
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count && i < 1024; ++i) {
        ModuleRow r;
        char      path[MAX_PATH] = {};
        if (GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, MAX_PATH)) {
            r.path = path;
            const auto slash = r.path.find_last_of("\\/");
            r.name = (slash == std::string::npos) ? r.path : r.path.substr(slash + 1);
        }
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) {
            r.base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
            r.size = mi.SizeOfImage;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

// Directory holding a given module, with a trailing backslash.
inline std::string ModuleDirectory(HMODULE h) {
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(h, path, MAX_PATH)) { return {}; }
    std::string s = path;
    const auto  slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string{} : s.substr(0, slash + 1);
}

}  // namespace mpf
