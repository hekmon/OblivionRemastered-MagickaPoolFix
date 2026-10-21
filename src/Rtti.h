// Rtti.h - MagickaPoolFix
//
// Locating the PlayerCharacter vtable through MSVC RTTI, so that no hard-coded
// game address is needed and the plugin keeps working across game patches.
//
// Chain followed (MSVC x64 layout):
//
//   1. The mangled type name ".?AVPlayerCharacter@@" appears verbatim in a data
//      section. It is the `name` member of a TypeDescriptor:
//
//          struct TypeDescriptor { void* vfptr; void* spare; char name[]; };
//
//      so the descriptor starts 16 bytes before the string.
//
//   2. A RTTICompleteObjectLocator references that descriptor by 32-bit RVA:
//
//          struct RTTICompleteObjectLocator {
//              uint32 signature;         // 1 on x64
//              uint32 offset;
//              uint32 cdOffset;
//              uint32 pTypeDescriptor;   // RVA
//              uint32 pClassDescriptor;  // RVA
//              uint32 pSelf;             // RVA of the locator itself
//          };
//
//      The pSelf field makes validation self-checking: a candidate is only
//      accepted when pSelf actually points back at the candidate.
//
//   3. A pointer to that locator sits immediately before the vtable, so the
//      vtable address is (address of that pointer) + 8.
//
// Every step is validated rather than assumed, and the number of candidates found
// at each step is reported so an ambiguous result can be spotted in the log
// instead of silently picking the wrong one.

#pragma once

#include "PCH.h"
#include "PeUtil.h"

namespace mpf {

struct VtableLocation {
    std::uintptr_t typeDescriptor = 0;
    std::uintptr_t locator        = 0;
    std::uintptr_t vtable         = 0;
    std::size_t    validEntries   = 0;  // leading entries pointing into code

    bool Valid() const { return vtable != 0; }
};

struct RttiSearchStats {
    int nameHits      = 0;
    int locatorHits   = 0;
    int vtableHits    = 0;
    int sectionsScanned = 0;
};

namespace detail {

// Search a byte range for an exact NUL-terminated string.
inline std::uintptr_t FindString(std::uintptr_t start, std::size_t size,
                                 const char* needle) {
    const std::size_t len = std::strlen(needle);
    if (size <= len) { return 0; }
    const auto* p   = reinterpret_cast<const char*>(start);
    const auto* end = p + (size - len);
    for (const char* c = p; c < end; ++c) {
        if (c[0] == needle[0] && std::memcmp(c, needle, len + 1) == 0) {
            return reinterpret_cast<std::uintptr_t>(c);
        }
    }
    return 0;
}

}  // namespace detail

// mangledName example: ".?AVPlayerCharacter@@"
inline VtableLocation FindVtableByRtti(const ModuleImage& img,
                                       const char*        mangledName,
                                       std::size_t        minEntries,
                                       RttiSearchStats&   stats) {
    VtableLocation out;

    // ---- step 1: the type descriptor -------------------------------------
    std::uintptr_t nameAddr = 0;
    for (const auto& s : img.Sections()) {
        if (s.Executable() || s.size == 0) { continue; }
        ++stats.sectionsScanned;
        const std::uintptr_t hit = detail::FindString(s.start, s.size, mangledName);
        if (hit) {
            ++stats.nameHits;
            if (!nameAddr) { nameAddr = hit; }
        }
    }
    if (!nameAddr) { return out; }
    // The string is TypeDescriptor::name, at offset 16 of the descriptor.
    out.typeDescriptor = nameAddr - 0x10;
    const std::uint32_t tdRva = img.Rva(out.typeDescriptor);

    // ---- step 2: the complete object locator ------------------------------
    // Scan read-only data for a dword equal to tdRva sitting at the
    // pTypeDescriptor slot, i.e. 12 bytes into a candidate locator.
    for (const auto& s : img.Sections()) {
        if (s.Executable() || s.size < 24) { continue; }
        const auto* base = reinterpret_cast<const std::uint8_t*>(s.start);
        for (std::size_t off = 12; off + 12 <= s.size; off += 4) {
            std::uint32_t v = 0;
            std::memcpy(&v, base + off, 4);
            if (v != tdRva) { continue; }

            const std::uintptr_t cand = s.start + off - 12;
            std::uint32_t sig = 0, self = 0;
            std::memcpy(&sig, reinterpret_cast<const void*>(cand), 4);
            std::memcpy(&self, reinterpret_cast<const void*>(cand + 20), 4);
            if (sig != 1) { continue; }             // x64 signature
            if (self != img.Rva(cand)) { continue; }  // self-consistency check

            ++stats.locatorHits;
            if (!out.locator) { out.locator = cand; }
        }
    }
    if (!out.locator) { return out; }

    // ---- step 3: the vtable ----------------------------------------------
    for (const auto& s : img.Sections()) {
        if (s.Executable() || s.size < 16) { continue; }
        const auto* base = reinterpret_cast<const std::uint8_t*>(s.start);
        const std::size_t limit = s.size - 8;
        for (std::size_t off = 0; off + 8 <= limit; off += 8) {
            std::uintptr_t v = 0;
            std::memcpy(&v, base + off, 8);
            if (v != out.locator) { continue; }

            const std::uintptr_t vt = s.start + off + 8;

            // Structural validation: count how many leading entries point into
            // an executable section. This does not prove the semantics of any
            // given slot, but it does prove the vtable is long enough for the
            // indices we intend to use, which is what protects against writing
            // or reading past its end.
            std::size_t good = 0;
            while (good < minEntries + 16) {
                const std::uintptr_t slot = vt + good * 8;
                if (!s.Contains(slot) || !s.Contains(slot + 7)) { break; }
                std::uintptr_t fn = 0;
                std::memcpy(&fn, reinterpret_cast<const void*>(slot), 8);
                if (!img.InExecutable(fn)) { break; }
                ++good;
            }

            ++stats.vtableHits;
            if (good > out.validEntries) {
                out.vtable       = vt;
                out.validEntries = good;
            }
        }
    }

    if (out.validEntries < minEntries) {
        // Report the find but refuse to hand back a vtable that is too short for
        // the indices we need.
        out.vtable = 0;
    }
    return out;
}

}  // namespace mpf
