// Follow-up: pin down whether the SERIAL dispatcher can drive the run-state,
// and with which command. Run after HRRemoteStart.java (same program).
//
// @category HarvestRight
// @menupath Tools.HarvestRight.StartDetail

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.util.task.ConsoleTaskMonitor;

import java.util.HashSet;
import java.util.Set;

public class HRStartDetail extends GhidraScript {

    final long DISPATCHER = 0x291b0L;     // FUN_000291b0 (references verb strings)
    final long SHARED_HANDLER = 0x218ecL; // FUN_000218ec (writes run-state = param)
    final long EXECUTOR = 0x404c8L;       // FUN_000404c8 (called with param_1)

    Listing listing;
    ReferenceManager refmgr;

    Address A(long x) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(x);
    }

    void decompile(long fnAddr, String label) {
        Function f = currentProgram.getFunctionManager().getFunctionContaining(A(fnAddr));
        println("\n===== " + label + " (0x" + Long.toHexString(fnAddr) + ") =====");
        if (f == null) { println("  (no function here)"); return; }
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults res = di.decompileFunction(f, 90, new ConsoleTaskMonitor());
        if (res != null && res.getDecompiledFunction() != null) {
            println(res.getDecompiledFunction().getC());
        }
    }

    @Override
    public void run() throws Exception {
        listing = currentProgram.getListing();
        refmgr = currentProgram.getReferenceManager();
        var fm = currentProgram.getFunctionManager();

        // Q1: does the dispatcher (or its callees) actually call the shared handler?
        Function handler = fm.getFunctionContaining(A(SHARED_HANDLER));
        println("Callers of shared handler 0x" + Long.toHexString(SHARED_HANDLER) + ":");
        Set<Address> callerFns = new HashSet<>();
        for (Reference ref : refmgr.getReferencesTo(handler.getEntryPoint())) {
            if (ref.getReferenceType().isCall()) {
                Function c = fm.getFunctionContaining(ref.getFromAddress());
                println("   called from " + (c == null ? "?" : c.getName())
                        + " @ " + ref.getFromAddress());
                if (c != null) callerFns.add(c.getEntryPoint());
            }
        }

        // Q2: show the dispatcher so we can see how it computes the command-ID
        //     (param) and whether it passes it toward the run-state write.
        decompile(DISPATCHER, "SERIAL DISPATCHER");

        // Q3: the executor called with param_1 — this likely maps command-ID to
        //     the actual action object (the (*(code**)(*piVar7+0x10))() call).
        decompile(EXECUTOR, "COMMAND EXECUTOR");

        println("\nDONE - read dispatcher to see which verbs yield which command-ID,");
        println("and whether any drive the run-state into a cycle-start value.");
    }
}
