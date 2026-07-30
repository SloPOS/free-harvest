# Ghidra headless post-analysis script.
# Question: can an INBOUND serial command start a freeze-dry run?
# Method: find writes to the run-state global (0x20003291) and report which are
# reachable (via the call graph) from the command dispatcher, versus only from
# touchscreen/UI code.
#
# Run via analyzeHeadless (see run_ghidra.ps1). Ghidra/Jython API.

# @category HarvestRight

from ghidra.program.model.symbol import RefType
from ghidra.util.task import ConsoleTaskMonitor

STATE_GLOBAL = 0x20003291
# String addresses of the inbound command verbs (from static analysis).
CMD_STRINGS = {
    "SENDBATCH": 0x86b4c, "SENDCUSTOM": 0x86b64, "SENDCANDY": 0x86b58,
    "REQSTAT": 0x86bb4, "SETPREF": 0x86bf8, "SETDATE": 0x86c00,
    "STATE": 0x86b3c, "STATUS": 0x86b34,
}
PSIS_STRING = 0x8367c  # "ProcessIncomingSerialStream"

fm = currentProgram.getFunctionManager()
refmgr = currentProgram.getReferenceManager()
listing = currentProgram.getListing()
af = currentProgram.getAddressFactory()
sp = af.getDefaultAddressSpace()


def addr(a):
    return sp.getAddress(a)


def func_containing(a):
    return fm.getFunctionContaining(a)


def callers_of(func, depth, seen):
    """Return set of functions that transitively call `func` (up to depth)."""
    out = set()
    if func is None or depth < 0 or func in seen:
        return out
    seen.add(func)
    entry = func.getEntryPoint()
    for ref in refmgr.getReferencesTo(entry):
        if ref.getReferenceType().isCall() or ref.getReferenceType().isFlow():
            c = func_containing(ref.getFromAddress())
            if c is not None:
                out.add(c)
                out |= callers_of(c, depth - 1, seen)
    return out


print("=" * 70)
print("RUN-STATE WRITE REACHABILITY ANALYSIS")
print("=" * 70)

# 1. Find the dispatcher function (contains a ref to the PSIS string or the
#    command-string table). We approximate: the function referencing SENDBATCH.
dispatch_funcs = set()
for name, saddr in CMD_STRINGS.items():
    for ref in refmgr.getReferencesTo(addr(saddr)):
        f = func_containing(ref.getFromAddress())
        if f is not None:
            dispatch_funcs.add(f)
print("\nFunctions referencing command-verb strings (dispatch layer):")
for f in dispatch_funcs:
    print("   %s @ %s" % (f.getName(), f.getEntryPoint()))

# Everything reachable UP from the dispatch (i.e. the dispatch and its callees'
# relationship) - we actually want callees of dispatch. Collect callees.
def callees_of(func, depth, seen):
    out = set()
    if func is None or depth < 0 or func in seen:
        return out
    seen.add(func)
    body = func.getBody()
    inst = listing.getInstructions(body, True)
    for i in inst:
        for ref in i.getReferencesFrom():
            if ref.getReferenceType().isCall():
                c = fm.getFunctionAt(ref.getToAddress())
                if c is not None:
                    out.add(c)
                    out |= callees_of(c, depth - 1, seen)
    return out

dispatch_closure = set(dispatch_funcs)
for f in list(dispatch_funcs):
    dispatch_closure |= callees_of(f, 6, set())
print("\nTotal functions reachable from dispatch (depth 6): %d"
      % len(dispatch_closure))

# 2. Find all writes to the state global.
print("\nWrites to run-state global 0x%X:" % STATE_GLOBAL)
writers = []
for ref in refmgr.getReferencesTo(addr(STATE_GLOBAL)):
    rt = ref.getReferenceType()
    if rt.isWrite():
        wf = func_containing(ref.getFromAddress())
        writers.append((ref.getFromAddress(), wf))

if not writers:
    print("   (no direct write refs found - global may be accessed via pointer;"
          " check data xrefs manually)")

reachable = []
for waddr, wf in writers:
    in_dispatch = wf in dispatch_closure
    tag = "  <== REACHABLE FROM SERIAL DISPATCH" if in_dispatch else ""
    fn = wf.getName() if wf else "?"
    print("   write @ %s in %s%s" % (waddr, fn, tag))
    if in_dispatch:
        reachable.append((waddr, wf))

print("\n" + "=" * 70)
if reachable:
    print("VERDICT: %d state-write(s) ARE reachable from the serial command "
          "dispatch." % len(reachable))
    print("=> Remote initiation of a state change is POSSIBLE. Inspect these "
          "handlers for the exact command + args:")
    for waddr, wf in reachable:
        print("     %s (%s)" % (wf.getName(), waddr))
else:
    print("VERDICT: NO write to the run-state global is reachable from the "
          "serial command dispatch.")
    print("=> The run state is set only by touchscreen/UI paths. Remote START "
          "is NOT supported by this firmware over the wire.")
print("=" * 70)
