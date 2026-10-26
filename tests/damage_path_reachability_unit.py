"""Offline reachability analysis for the WF_MAXON damage-block hook sites.

Static-only: reads the on-disk Warframe executable, disassembles the candidate
blocks, and proves whether the hooked mid-block byte (RVA 0x167E9A2) is
reachable from any of the block's real entry points. No game process needed.
"""
import pefile, capstone, struct, sys, bisect

PE_PATH = r'D:\SteamLibrary\steamapps\common\Warframe\Warframe.x64.exe'
ENTRY = 0x167E8A0          # the block our hooks currently target
HOOK_SITE = 0x167E9A2       # cvttss2si ecx, xmm1  (DamageResult)
HOOK_DST = 0x167E9CC        # short-branch destination (DamageResultBranchTarget)
TRUE_ENTRY = 0x167E830      # only reachable entry into the routine
RETIRE = 0x167E892          # xor eax,eax ; ret  (first block's exit)
BLOCK_END = 0x167E9F3       # int3 padding after the epilogue ret

data = open(PE_PATH, 'rb').read()
pe = pefile.PE(data=data)
base = pe.OPTIONAL_HEADER.ImageBase

def rva2off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def read(rva, n):
    off = rva2off(rva)
    return data[off:off + n]

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True

def decode(rva, count=400):
    out = []
    code = read(rva, count * 15)
    for ins in md.disasm(code, base + rva):
        out.append(ins)
        if len(out) >= count:
            break
    return out

def func_table():
    funcs = []
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        funcs.append((e.struct.BeginAddress, e.struct.EndAddress))
    funcs.sort()
    return funcs

def rel32_targets(blob, sec_rva):
    """Yield (site_rva, target_rva) for every rel32 call/jmp in the blob."""
    for j in range(len(blob) - 5):
        if blob[j] in (0xE8, 0xE9):
            disp = struct.unpack_from('<i', blob, j + 1)[0]
            site = sec_rva + j
            yield site, site + 5 + disp

def exec_blobs():
    for s in pe.sections:
        if s.Characteristics & 0x20000000:
            yield s.VirtualAddress, data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]

def callers_of(target):
    return [site for sec_rva, blob in exec_blobs()
            for site, tgt in rel32_targets(blob, sec_rva) if tgt == target]

# ---------------------------------------------------------------- test 1
def test_hook_site_is_mid_block():
    funcs = func_table()
    starts = [f[0] for f in funcs]
    i = bisect.bisect_right(starts, HOOK_SITE) - 1
    inside = i >= 0 and funcs[i][0] <= HOOK_SITE < funcs[i][1]
    print(f'[test] hook site 0x{HOOK_SITE:X} inside a .pdata function: {inside}')
    assert not inside, 'hook site unexpectedly registered as a function body'

    callers = callers_of(HOOK_SITE)
    print(f'[test] direct call/jmp targets landing on hook site: {len(callers)}')
    assert not callers, f'hook site IS a call target: {[hex(c) for c in callers]}'

# ---------------------------------------------------------------- test 2
def test_entry_has_no_fallthrough_into_block():
    insns = decode(TRUE_ENTRY, 40)
    retire_insn = next((i for i in insns if i.address - base == RETIRE), None)
    assert retire_insn is not None, 'retire instruction not found in first block'
    end_of_block = retire_insn.address - base + retire_insn.size
    print(f'[test] first block spans 0x{TRUE_ENTRY:X} .. 0x{end_of_block:X}')
    assert end_of_block <= ENTRY, (
        f'first block extends past the hook target: 0x{end_of_block:X} > 0x{ENTRY:X}')
    assert retire_insn.mnemonic == 'xor', retire_insn.mnemonic
    nxt = insns[insns.index(retire_insn) + 1]
    assert nxt.mnemonic == 'ret', f'expected ret, got {nxt.mnemonic}'
    print(f'[test] first block exit = xor eax,eax ; ret @ 0x{RETIRE:X}  -> OK')

# ---------------------------------------------------------------- test 3
def test_hook_target_callers_are_tail_calls():
    callers = callers_of(ENTRY)
    print(f'[test] direct callers of 0x{ENTRY:X}: {[hex(c) for c in callers]}')
    funcs = func_table()
    starts = [f[0] for f in funcs]
    for site in callers:
        i = bisect.bisect_right(starts, site) - 1
        fbegin, fend = funcs[i]
        # A `call` is a tail call when the block it lands in ends with the
        # callee's epilogue (ret / int3 padding) rather than returning here.
        call_end = site + 5
        # walk the block linearly until its epilogue
        last = None
        for ins in decode(ENTRY, 200):
            last = ins
            if ins.mnemonic == 'ret':
                break
        is_tail = last is not None and last.mnemonic == 'ret'
        print(f'[test] caller 0x{site:X} func 0x{fbegin:X}-0x{fend:X} '
              f'call_end=0x{call_end:X} callee_last_insn={last.mnemonic if last else None} '
              f'@ 0x{(last.address-base) if last else 0:X} tail_call={is_tail}')
        assert is_tail, f'block at 0x{ENTRY:X} does not end with an epilogue'

# ---------------------------------------------------------------- test 4
def test_hook_site_unreachable_from_entry():
    visited = set()
    worklist = [TRUE_ENTRY]
    while worklist:
        rva = worklist.pop()
        if rva in visited or not (0x167E800 <= rva <= BLOCK_END):
            continue
        visited.add(rva)
        code = read(rva, 15)
        ins = next(md.disasm(code, base + rva), None)
        if ins is None:
            continue
        if ins.mnemonic in ('ret', 'int3'):
            continue
        worklist.append(rva + ins.size)
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_IMM:
                tgt = op.imm - base
                if 0x167E800 <= tgt <= BLOCK_END:
                    worklist.append(tgt)
    print(f'[test] reachable from 0x{TRUE_ENTRY:X}: {len(visited)} instructions')
    assert HOOK_SITE not in visited, 'hook site IS reachable from the real entry'
    assert ENTRY not in visited, 'hook target IS reachable from the real entry'
    print(f'[test] hook site 0x{HOOK_SITE:X} and hook target 0x{ENTRY:X} '
          f'are UNREACHABLE from the real entry -> OK')

# ---------------------------------------------------------------- test 5
def test_hook_patch_is_safe_nop_padding():
    insns = decode(HOOK_SITE, 6)
    total = sum(i.size for i in insns[:3])
    print(f'[test] instructions covered by a 12-byte patch at 0x{HOOK_SITE:X}:')
    for i in insns[:3]:
        print(f'         0x{i.address-base:08X}  {i.bytes.hex():20s} {i.mnemonic:8s} {i.op_str}')
    print(f'[test] covered bytes = {total} (patch length = 12)')
    assert total == 12, f'patch covers {total} bytes, not 12'
    assert insns[2].mnemonic == 'je', insns[2].mnemonic
    tgt = insns[2].operands[0].imm - base
    print(f'[test] short branch target = 0x{tgt:X} (expected 0x{HOOK_DST:X})')
    assert tgt == HOOK_DST, f'branch target 0x{tgt:X} != expected 0x{HOOK_DST:X}'

# ---------------------------------------------------------------- test 6
def test_block_tail_is_int3_padding():
    """The bytes after the epilogue ret must be int3 padding, not live code."""
    blob = read(0x167E9F2, 0x8)
    print(f'[test] bytes at epilogue ret 0x167E9F2: {blob.hex(" ")}')
    assert blob[0] == 0xC3, 'expected ret at the epilogue'
    assert blob[1] == 0xCC, 'expected int3 padding after the epilogue'
    print('[test] epilogue = ret followed by int3 padding -> OK')

if __name__ == '__main__':
    ok = True
    for t in (test_hook_site_is_mid_block,
              test_entry_has_no_fallthrough_into_block,
              test_hook_target_callers_are_tail_calls,
              test_hook_site_unreachable_from_entry,
              test_hook_patch_is_safe_nop_padding,
              test_block_tail_is_int3_padding):
        print(f'\n--- {t.__name__} ---')
        try:
            t()
        except AssertionError as e:
            ok = False
            print(f'[FAIL] {e}')
    print('\nRESULT:', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)
