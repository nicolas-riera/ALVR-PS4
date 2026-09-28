"""Minimal PS4 PRX/SPRX (decrypted ELF) inspector.

  python tools/re/prx.py exports <module.sprx>             list exported NIDs
  python tools/re/prx.py disasm <module.sprx> <name|NID> [n]  disassemble n instructions (default 200)
  python tools/re/prx.py nid <name>                          compute the NID of a symbol name
  python tools/re/prx.py imports <module.sprx>               list imported NIDs (resolved names when known)
"""
import base64
import hashlib
import struct
import sys

NID_SUFFIX = bytes.fromhex("518D64A635DED8C1E6B039B1C3E55230")
PT_LOAD = 1
PROLOGUE = bytes([0x55, 0x48, 0x89, 0xE5])  # push rbp; mov rbp, rsp
CALL_REL32 = bytes([0xE8])
PLT_JMP = bytes([0xFF, 0x25])  # jmp qword ptr [rip+disp32]
PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMTABSZ = 0x6100003F


def nid(name: str) -> str:
    h = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    return base64.b64encode(h[:8][::-1]).decode().rstrip("=").replace("/", "-")


class Prx:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        assert d[:4] == b"\x7fELF", "not a decrypted ELF"
        phoff, = struct.unpack_from("<Q", d, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
        self.loads = []
        dyn = dynlib = None
        for i in range(phnum):
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
                "<IIQQQQQQ", d, phoff + i * phentsize)
            if p_type == PT_LOAD or p_type == 0x61000010:  # PT_SCE_RELRO
                self.loads.append((p_vaddr, p_offset, p_filesz, p_flags))
            elif p_type == PT_DYNAMIC:
                dyn = (p_offset, p_filesz)
            elif p_type == PT_SCE_DYNLIBDATA:
                dynlib = (p_offset, p_filesz)
        tags = {}
        off, size = dyn
        for j in range(size // 16):
            tag, val = struct.unpack_from("<qQ", d, off + j * 16)
            tags.setdefault(tag, val)
        base = dynlib[0]
        self.strtab = base + tags[DT_SCE_STRTAB]
        symtab = base + tags[DT_SCE_SYMTAB]
        symsz = tags[DT_SCE_SYMTABSZ]
        self.syms = []
        for k in range(symsz // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", d, symtab + k * 24)
            name = d[self.strtab + st_name:d.index(b"\0", self.strtab + st_name)].decode()
            self.syms.append((name, st_info, st_shndx, st_value, st_size))
        self.jmprel = (base + tags.get(DT_SCE_JMPREL, 0), tags.get(DT_SCE_PLTRELSZ, 0))

    def exports(self):
        return [(n, v, s) for (n, info, shndx, v, s) in self.syms if shndx != 0 and v]

    def vaddr_to_off(self, va):
        for vaddr, off, filesz, flags in self.loads:
            if vaddr <= va < vaddr + filesz:
                return off + (va - vaddr)
        raise ValueError(hex(va))

    def plt_stubs(self):
        """Map import name -> PLT stub vaddr (jmp qword ptr [rip+disp] to the GOT slot)."""
        slots = self.plt_targets()
        out = {}
        for vaddr, off, filesz, flags in self.loads:
            if not flags & 1:  # executable only
                continue
            seg = self.data[off:off + filesz]
            i = seg.find(PLT_JMP)
            while i != -1:
                disp, = struct.unpack_from("<i", seg, i + 2)
                target = vaddr + i + 6 + disp
                if target in slots:
                    out.setdefault(slots[target], vaddr + i)
                i = seg.find(PLT_JMP, i + 1)
        return out

    def callers(self, target_va):
        """Vaddrs of direct `call rel32` instructions to target_va, with a guessed function start
        (nearest preceding `push rbp; mov rbp, rsp`)."""
        res = []
        for vaddr, off, filesz, flags in self.loads:
            if not flags & 1:
                continue
            seg = self.data[off:off + filesz]
            i = seg.find(CALL_REL32)
            while i != -1:
                if i + 5 <= len(seg):
                    rel, = struct.unpack_from("<i", seg, i + 1)
                    if vaddr + i + 5 + rel == target_va:
                        start = seg.rfind(PROLOGUE, max(0, i - 0x20000), i)
                        res.append((vaddr + i, vaddr + start if start != -1 else None))
                i = seg.find(CALL_REL32, i + 1)
        return res

    def plt_targets(self):
        """Map import-slot vaddr -> symbol name, from the JMPREL relocations."""
        off, size = self.jmprel
        out = {}
        for k in range(size // 24):
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", self.data, off + k * 24)
            sym = r_info >> 32
            if sym < len(self.syms):
                out[r_offset] = self.syms[sym][0]
        return out


def known_names():
    """NID -> readable name for symbols mentioned in the reference sources."""
    import os, re, glob
    names = {}
    ref = os.path.join(os.path.dirname(__file__), "..", "..", "reference")
    files = glob.glob(os.path.join(ref, "shadps4", "*.cpp")) +         glob.glob(os.path.join(ref, "shadps4-src", "src", "core", "libraries", "**", "*.cpp"), recursive=True)
    for f in files:
        for m in re.finditer(r'LIB_FUNCTION\("([^"]+)",\s*"[^"]*",\s*\d+,\s*"[^"]*",\s*(\w+)\)', open(f, encoding='utf-8', errors='replace').read()):
            names[m.group(1)] = m.group(2)
    return names


def main():
    cmd = sys.argv[1]
    if cmd == "nid":
        print(nid(sys.argv[2]))
        return
    prx = Prx(sys.argv[2])
    names = known_names()
    if cmd == "exports":
        for n, v, s in sorted(prx.exports(), key=lambda x: x[1]):
            short = n.split("#")[0]
            print(f"0x{v:08x} size=0x{s:x} {n:24} {names.get(short, '')}")
    elif cmd == "callers":
        stubs = prx.plt_stubs()
        for want in sys.argv[3:]:
            hits = [(n, va) for n, va in stubs.items() if names.get(n.split("#")[0]) == want or n.split("#")[0] == want]
            for n, va in hits:
                for call, start in prx.callers(va):
                    print(f"{want} call=0x{call:x} func=0x{start:x}" if start else f"{want} call=0x{call:x} func=?")
    elif cmd == "stubs":
        for n, va in sorted(prx.plt_stubs().items(), key=lambda x: x[1]):
            print(f"0x{va:08x} {n:24} {names.get(n.split('#')[0], '')}")
    elif cmd == "imports":
        for va, n in sorted(prx.plt_targets().items()):
            print(f"0x{va:08x} {n:24} {names.get(n.split('#')[0], '')}")
    elif cmd == "at":
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        va = int(sys.argv[3], 16)
        count = int(sys.argv[4]) if len(sys.argv) > 4 else 60
        off = prx.vaddr_to_off(va)
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        for i, ins in enumerate(md.disasm(prx.data[off:off + 4096], va)):
            print(f"  0x{ins.address:08x}: {ins.mnemonic:8} {ins.op_str}")
            if i + 1 >= count or ins.mnemonic == "ret":
                break
    elif cmd == "disasm":
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
        target = sys.argv[3]
        want = target if len(target) == 11 else nid(target)
        count = int(sys.argv[4]) if len(sys.argv) > 4 else 200
        hits = [(n, v, s) for (n, v, s) in prx.exports() if n.split("#")[0] == want]
        if not hits:
            sys.exit(f"{target} ({want}) not exported")
        n, va, size = hits[0]
        off = prx.vaddr_to_off(va)
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        print(f"; {target} nid={want} va=0x{va:x} size=0x{size:x}")
        for i, ins in enumerate(md.disasm(prx.data[off:off + max(size, 16) if size else off + 4096], va)):
            print(f"  0x{ins.address:08x}: {ins.mnemonic:8} {ins.op_str}")
            if i + 1 >= count:
                break


if __name__ == "__main__":
    main()
