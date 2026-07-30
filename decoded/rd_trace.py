#!/usr/bin/env python3
"""
Recursive-descent Cortex-M analyzer (no Ghidra needed).

Goal: determine whether an inbound serial command can reach a write to the
run-state global, i.e. whether the firmware supports remote START.

Approach:
  1. Build a function-boundary + call-graph by recursive descent from seed
     entry points (reset vector + the command dispatcher), following BL/B/CBZ
     and TBB/TBH jump tables. This is accurate where linear sweep is not.
  2. Locate every instruction that STORES to the state global (found via
     PC-relative literal loads of its address).
  3. Report which storing-functions are reachable from the command dispatch
     subgraph.
"""
import struct
from capstone import *
from capstone.arm import *

BASE = 0x18000
blob = open(r"C:\Users\Jacob\ghidra\fw.bin", "rb").read()
END = BASE + len(blob)

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
md.detail = True


def rd(addr, n=4):
    off = addr - BASE
    if off < 0 or off + n > len(blob):
        return None
    return blob[off:off + n]


def word(addr):
    b = rd(addr, 4)
    return struct.unpack("<I", b)[0] if b else None


def in_range(a):
    return BASE <= a < END


# ---- recursive-descent disassembly of one function -------------------------
def disasm_func(entry, func_insns, max_bytes=0x2000):
    """Linear-within-basic-block + follow local branches, collect instructions
    and call targets. Returns (set(call_targets), set(store_addrs_to_state))."""
    entry &= ~1
    if entry in func_insns:
        return func_insns[entry]
    calls = set()
    worklist = [entry]
    visited = set()
    while worklist:
        pc = worklist.pop()
        if pc in visited or not in_range(pc):
            continue
        code = rd(pc, min(max_bytes, END - pc))
        if not code:
            continue
        for ins in md.disasm(code, pc):
            if ins.address in visited:
                break
            visited.add(ins.address)
            m = ins.mnemonic
            # calls
            if m in ("bl", "blx"):
                for op in ins.operands:
                    if op.type == ARM_OP_IMM:
                        calls.add(op.imm & ~1)
            # unconditional end-of-block
            if m in ("b", "bx", "pop") and ("pc" in ins.op_str or m == "b"):
                if m == "b":
                    for op in ins.operands:
                        if op.type == ARM_OP_IMM and in_range(op.imm):
                            worklist.append(op.imm & ~1)
                break
            # conditional branch: follow target + fallthrough
            if m.startswith("b") and m not in ("bl", "blx", "bx", "bkpt") and ins.operands:
                for op in ins.operands:
                    if op.type == ARM_OP_IMM and in_range(op.imm):
                        worklist.append(op.imm & ~1)
            if m in ("cbz", "cbnz"):
                for op in ins.operands:
                    if op.type == ARM_OP_IMM and in_range(op.imm):
                        worklist.append(op.imm & ~1)
            # TBB/TBH jump tables: parse the inline table
            if m in ("tbb", "tbh"):
                _follow_table(ins, worklist)
                break
        # else fell through
    func_insns[entry] = calls
    return calls


def _follow_table(ins, worklist):
    # tbb [pc, rX] : table of bytes right after the instruction
    tbl = ins.address + ins.size
    # heuristic: read up to 24 entries, stop when target out of range
    size = 1 if ins.mnemonic == "tbb" else 2
    for i in range(48):
        ea = tbl + i * size
        raw = rd(ea, size)
        if not raw:
            break
        val = raw[0] if size == 1 else struct.unpack("<H", raw)[0]
        tgt = ins.address + 4 + 2 * val
        if in_range(tgt):
            worklist.append(tgt & ~1)
        else:
            break


# ---- find stores to the state global --------------------------------------
def find_state_writers(state_addr):
    """Scan every function for a PC-relative load of state_addr followed by a
    store through that register. Returns set of function entries (approx)."""
    # First find all literal-pool offsets holding state_addr.
    pools = [BASE + o for o in range(0, len(blob) - 4, 2)
             if struct.unpack_from("<I", blob, o)[0] == state_addr]
    writers = set()
    detail = []
    for pool in pools:
        # disassemble a window before the pool; track ldr rX,[pc,#d]->pool then str
        start = max(BASE, pool - 0x600)
        code = rd(start, pool - start + 4)
        regs = {}
        for ins in md.disasm(code, start):
            if ins.id == 0:
                continue
            m = ins.mnemonic
            if m == "ldr" and len(ins.operands) == 2 and \
               ins.operands[1].type == ARM_OP_MEM and \
               ins.operands[1].mem.base == ARM_REG_PC:
                pcv = (ins.address + 4) & ~3
                if pcv + ins.operands[1].mem.disp == pool:
                    regs[ins.operands[0].reg] = ins.address
            elif m in ("str", "strb", "strh"):
                if ins.operands and ins.operands[-1].type == ARM_OP_MEM:
                    if ins.operands[-1].mem.base in regs:
                        detail.append(ins.address)
    return detail


# ---- reachability ----------------------------------------------------------
def reachable_from(entry, func_insns, limit=4000):
    seen = set()
    wl = [entry & ~1]
    while wl and len(seen) < limit:
        f = wl.pop()
        if f in seen:
            continue
        seen.add(f)
        for c in disasm_func(f, func_insns):
            if c not in seen:
                wl.append(c)
    return seen


def which_func(addr, func_entries):
    """Nearest function entry <= addr."""
    best = None
    for e in func_entries:
        if e <= addr and (best is None or e > best):
            best = e
    return best


if __name__ == "__main__":
    import json
    print("Recursive-descent trace starting...")

    # Command dispatcher: the code region right after the verb-string table
    # (identified statically at 0x29728). Seed from there.
    DISPATCH = 0x29728
    RESET = word(BASE + 4)  # reset vector (thumb entry) if vector table at BASE
    print("reset vector:", hex(RESET) if RESET else "?")

    func_insns = {}
    disp_reach = reachable_from(DISPATCH, func_insns)
    print("functions reachable from serial dispatch:", len(disp_reach))

    # candidate state globals to test (from capstone earlier + scan for SRAM ptrs
    # heavily referenced). Test the prime suspect first.
    for STATE in (0x20003291, 0x20003290, 0x20003292):
        writes = find_state_writers(STATE)
        if not writes:
            continue
        func_entries = set(func_insns.keys())
        reach_writes = [w for w in writes
                        if which_func(w, func_entries) in disp_reach]
        print(f"\nstate {hex(STATE)}: {len(writes)} store(s), "
              f"{len(reach_writes)} reachable from dispatch")
        for w in writes:
            f = which_func(w, func_entries)
            tag = " <== REACHABLE FROM SERIAL" if f in disp_reach else ""
            print(f"   store@{hex(w)} in func {hex(f) if f else '?'}{tag}")

    print("\nDONE")
