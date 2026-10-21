#!/usr/bin/env python3
"""First look at a Windows minidump (.dmp), without a debugger or symbols.

Prints the loaded modules, the exception record (code, address, owning module and
RVA, PE timestamp), and for an access violation the faulting address and whether
it lies in a module. Then scans the crashing thread's stack for words that point
into a module: a raw scan, not an unwind, so expect false positives.

This is how the virtual-call crash of 0.4.0-probe was traced to RVA 0x458F57C in
the game executable (docs/FIELD-JOURNAL.md; docs/SPEC.md section 9.1).

Usage: minidump-triage.py <file.dmp>
"""
import struct, sys

if len(sys.argv) != 2:
    sys.exit(__doc__.strip().splitlines()[-1])

path = sys.argv[1]
d = open(path, 'rb').read()
sig, ver, nstreams, dirrva, chk, ts, flags = struct.unpack_from('<4sIIIIIQ', d, 0)
assert sig == b'MDMP', sig
print(f"signature {sig!r}  streams {nstreams}  timestamp {ts}")

streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from('<III', d, dirrva + i * 12)
    streams[t] = (size, rva)
print("stream types:", sorted(streams))


def mdstring(rva):
    (ln,) = struct.unpack_from('<I', d, rva)
    return d[rva + 4:rva + 4 + ln].decode('utf-16-le', 'replace')


# ---- modules
mods = []
if 4 in streams:
    _, rva = streams[4]
    (n,) = struct.unpack_from('<I', d, rva)
    for i in range(n):
        o = rva + 4 + i * 108
        base, size, _c, mts, nrva = struct.unpack_from('<QIIII', d, o)
        mods.append((base, size, mdstring(nrva), mts))
    mods.sort()
    print(f"\n{len(mods)} modules in the dump")


def owner(addr):
    for base, size, name, mts in mods:
        if base <= addr < base + size:
            return name, base, addr - base, mts
    return None


# ---- exception
if 6 in streams:
    _, rva = streams[6]
    tid, _a = struct.unpack_from('<II', d, rva)
    code, eflags, erec, eaddr, nparam, _u = struct.unpack_from('<IIQQII', d, rva + 8)
    params = struct.unpack_from('<15Q', d, rva + 8 + 32)
    names = {0xC0000005: 'EXCEPTION_ACCESS_VIOLATION',
             0xC0000094: 'INT_DIVIDE_BY_ZERO',
             0xC00000FD: 'STACK_OVERFLOW',
             0x80000003: 'BREAKPOINT',
             0xC000001D: 'ILLEGAL_INSTRUCTION',
             0xE06D7363: 'C++ exception (MSVC)'}
    print(f"\n=== EXCEPTION ===")
    print(f"thread id        : {tid} (0x{tid:X})")
    print(f"code             : 0x{code:08X}  {names.get(code, 'unknown')}")
    print(f"exception address: 0x{eaddr:016X}")
    o = owner(eaddr)
    if o:
        print(f"  -> module      : {o[0]}  base 0x{o[1]:X}  RVA 0x{o[2]:X}"
              f"  PE timestamp 0x{o[3]:X}")
    else:
        print("  -> module      : NOT INSIDE ANY LOADED MODULE")
    if code == 0xC0000005 and nparam >= 2:
        kind = {0: 'read', 1: 'write', 8: 'execute'}.get(params[0], params[0])
        print(f"  operation      : {kind}")
        print(f"  faulting addr  : 0x{params[1]:016X}")
        o2 = owner(params[1])
        print(f"  that address is: {o2[0]+' RVA 0x%X' % o2[2] if o2 else 'not in any module (heap/freed/unmapped)'}")

# ---- crashing thread stack: raw scan for return addresses
if 3 in streams and 6 in streams:
    _, trva = streams[3]
    (n,) = struct.unpack_from('<I', d, trva)
    for i in range(n):
        o = trva + 4 + i * 48
        t_id, sc, pri_c, pri, teb, sb_start, sb_size, sb_rva = struct.unpack_from('<IIIIQQII', d, o)
        if t_id != tid:
            continue
        print(f"\n=== stack of thread 0x{t_id:X} : {sb_size} bytes captured ===")
        raw = d[sb_rva:sb_rva + sb_size]
        seen, out = set(), []
        for k in range(0, len(raw) - 8, 8):
            (v,) = struct.unpack_from('<Q', raw, k)
            ow = owner(v)
            if ow and v not in seen:
                seen.add(v)
                out.append((sb_start + k, v, ow))
        print(f"{len(out)} code-pointer-shaped words (raw scan, NOT a real unwind):")
        for slot, v, ow in out[:40]:
            print(f"  [rsp+0x{slot - sb_start:05X}] 0x{v:016X}  {ow[0]} RVA 0x{ow[2]:X}")
        break
