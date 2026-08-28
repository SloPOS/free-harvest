# HarvestRight remote-start analysis — run inside the Ghidra GUI.
#
# HOW TO RUN:
#   1. In Ghidra: File > Import File > select fw.bin
#      (the decoded image - see tools/decode_h6r.py). When asked:
#         Language/Processor:  ARM:LE:32:Cortex   (little-endian Cortex-M)
#         Options > Block name/base: leave default, then set image base below.
#   2. Let auto-analysis run to completion (Analysis > Auto Analyze, accept defaults).
#   3. Set the image base if it is not already 0x18000:
#         Memory Map (window) > set base of the block to 0x00018000.
#      (Or import with "Base Address" = 0x18000 in the import options.)
#   4. Window > Script Manager > (folder icon) add this file's directory >
#      run  hr_ghidra_analysis.py.  Output appears in the Console.
#
# It answers: can an INBOUND serial command reach the code that starts a
# freeze-dry run? It does this from Ghidra's ACCURATE call graph + xrefs, which
# is what hand-rolled disassembly could not do reliably.

from ghidra.program.model.symbol import RefType

BASE = 0x18000

# --- Anchors recovered from static analysis (verified against the string table).
# Inbound command verb strings live in a contiguous table; these are a few of them.
CMD_STRING_ADDRS = [0x86b34, 0x86b3c, 0x86b4c, 0x86b58, 0x86b64, 0x86bb4,
                    0x86bf8, 0x86c00]  # STATUS STATE SENDBATCH SENDCANDY
#                                        SENDCUSTOM REQSTAT SETPREF SETDATE
SHARED_HANDLER = 0x218ec   # command handler that gates on the run-state global
STATE_GLOBAL_GUESS = 0x20003291

fm = currentProgram.getFunctionManager()
refmgr = currentProgram.getReferenceManager()
listing = currentProgram.getListing()
sp = currentProgram.getAddressFactory().getDefaultAddressSpace()


def A(x):
    return sp.getAddress(x)


def fentry(a):
    f = fm.getFunctionContaining(A(a))
    return f


def name(f):
    return f.getName() if f else "?"


def callers(func, depth=8, seen=None):
    if seen is None:
        seen = set()
    out = set()
    if func is None or depth < 0 or func in seen:
        return out
    seen.add(func)
    for ref in refmgr.getReferencesTo(func.getEntryPoint()):
        if ref.getReferenceType().isCall():
            c = fm.getFunctionContaining(ref.getFromAddress())
            if c:
                out.add(c)
                out |= callers(c, depth - 1, seen)
    return out


def callees(func, depth=8, seen=None):
    if seen is None:
        seen = set()
    out = set()
    if func is None or depth < 0 or func in seen:
        return out
    seen.add(func)
    it = listing.getInstructions(func.getBody(), True)
    while it.hasNext():
        ins = it.next()
        for ref in ins.getReferencesFrom():
            if ref.getReferenceType().isCall():
                c = fm.getFunctionAt(ref.getToAddress())
                if c:
                    out.add(c)
                    out |= callees(c, depth - 1, seen)
    return out


print("=" * 72)
print("HARVESTRIGHT REMOTE-START REACHABILITY (Ghidra accurate analysis)")
print("=" * 72)

# 1. Identify the dispatcher: functions that reference the command-verb strings.
disp = set()
for sa in CMD_STRING_ADDRS:
    for ref in refmgr.getReferencesTo(A(sa)):
        f = fm.getFunctionContaining(ref.getFromAddress())
        if f:
            disp.add(f)
print("\n[1] Dispatcher function(s) referencing command verbs:")
for f in disp:
    print("      %-28s @ %s" % (name(f), f.getEntryPoint()))
if not disp:
    print("      NONE FOUND -> auto-analysis may not have created these xrefs.")
    print("      Make sure analysis finished and the image base is 0x18000.")

# 2. The serial-reachable code = dispatcher + everything it can call.
serial_closure = set(disp)
for f in list(disp):
    serial_closure |= callees(f, 8)
# also include the shared handler's closure (it IS the command executor)
sh = fm.getFunctionContaining(A(SHARED_HANDLER))
if sh:
    serial_closure.add(sh)
    serial_closure |= callees(sh, 8)
print("\n[2] Functions reachable from serial dispatch: %d" % len(serial_closure))

# 3. Find the REAL run-state global: the memory the shared handler writes.
#    Look at data refs FROM the shared handler + its immediate callees.
def data_writes_in(func):
    ws = {}
    if not func:
        return ws
    it = listing.getInstructions(func.getBody(), True)
    while it.hasNext():
        ins = it.next()
        for ref in ins.getReferencesFrom():
            if ref.getReferenceType().isWrite():
                to = ref.getToAddress().getOffset()
                if 0x20000000 <= to < 0x20080000:  # SRAM
                    ws.setdefault(to, []).append(ins.getAddress())
    return ws

print("\n[3] SRAM globals WRITTEN by the shared command handler 0x%X:" % SHARED_HANDLER)
sh_writes = data_writes_in(sh)
for g in sorted(sh_writes):
    print("      %s  (written at %s)" % (hex(g), ", ".join(str(a) for a in sh_writes[g][:4])))
if STATE_GLOBAL_GUESS in sh_writes:
    print("      -> confirms run-state global guess 0x%X is written here." % STATE_GLOBAL_GUESS)

# 4. For each SRAM global the handler writes, check who ELSE writes it. If a
#    global is written by BOTH the serial path and the touchscreen "start"
#    path, that is the remote-control lever.
print("\n[4] Cross-reference of handler-written globals:")
verdict_globals = []
for g in sorted(sh_writes):
    writers = set()
    for ref in refmgr.getReferencesTo(A(g)):
        if ref.getReferenceType().isWrite():
            wf = fm.getFunctionContaining(ref.getFromAddress())
            if wf:
                writers.add(wf)
    serial_writers = [w for w in writers if w in serial_closure]
    ui_writers = [w for w in writers if w not in serial_closure]
    print("   global %s: %d writer-funcs (%d serial-reachable, %d other)"
          % (hex(g), len(writers), len(serial_writers), len(ui_writers)))
    verdict_globals.append((g, serial_writers, ui_writers))

# 5. Decompile the shared handler so we can READ what the command does.
print("\n[5] Decompiled shared command handler (read this to see the logic):")
try:
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor
    di = DecompInterface()
    di.openProgram(currentProgram)
    if sh:
        res = di.decompileFunction(sh, 60, ConsoleTaskMonitor())
        if res and res.getDecompiledFunction():
            print(res.getDecompiledFunction().getC())
except Exception as e:
    print("   (decompiler unavailable: %s)" % e)

print("=" * 72)
print("INTERPRETATION:")
print(" - If a handler-written global is set from BOTH serial-reachable code")
print("   and the run-state machine, remote initiation is POSSIBLE; read the")
print("   decompiled handler above for the exact command + argument gating.")
print(" - If the only serial-reachable writes just store status/echo flags and")
print("   the RUN state is written solely by touchscreen functions, remote")
print("   START is NOT supported over the wire.")
print("=" * 72)
