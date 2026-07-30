// HarvestRight remote-start analysis — Ghidra Java script (no Python needed).
//
// HOW TO RUN:
//   1. Import fw.bin as ARM:LE:32:Cortex with Base Address 0x18000, auto-analyze.
//   2. Window > Script Manager > Manage Script Directories (folder icon) > add
//        C:\Users\Jacob\Downloads\HarvestRight-v6.0.641041-Upgrade\decoded
//   3. Select HRRemoteStart.java in the list > Run (green arrow).
//      Output goes to the Console.
//
// Answers: can an INBOUND serial command reach the code that starts a
// freeze-dry run? Uses Ghidra's accurate call graph + xrefs.
//
// @category HarvestRight
// @menupath Tools.HarvestRight.RemoteStart

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.util.task.ConsoleTaskMonitor;

import java.util.HashSet;
import java.util.Set;
import java.util.TreeSet;

public class HRRemoteStart extends GhidraScript {

    // Inbound command verb strings (a subset of the dispatch table).
    // STATUS STATE SENDBATCH SENDCANDY SENDCUSTOM REQSTAT SETPREF SETDATE
    final long[] CMD_STRING_ADDRS = {
        0x86b34L, 0x86b3cL, 0x86b4cL, 0x86b58L, 0x86b64L, 0x86bb4L,
        0x86bf8L, 0x86c00L
    };
    final long SHARED_HANDLER = 0x218ecL;   // gates on the run-state global
    final long STATE_GLOBAL_GUESS = 0x20003291L;

    FunctionManager fm;
    ReferenceManager refmgr;
    Listing listing;

    Address A(long x) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(x);
    }

    Function fContaining(Address a) {
        return fm.getFunctionContaining(a);
    }

    String nm(Function f) {
        return f == null ? "?" : f.getName();
    }

    void callees(Function func, int depth, Set<Function> out, Set<Function> seen) {
        if (func == null || depth < 0 || seen.contains(func)) return;
        seen.add(func);
        InstructionIterator it = listing.getInstructions(func.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            for (Reference ref : ins.getReferencesFrom()) {
                if (ref.getReferenceType().isCall()) {
                    Function c = fm.getFunctionAt(ref.getToAddress());
                    if (c != null) {
                        out.add(c);
                        callees(c, depth - 1, out, seen);
                    }
                }
            }
        }
    }

    @Override
    public void run() throws Exception {
        fm = currentProgram.getFunctionManager();
        refmgr = currentProgram.getReferenceManager();
        listing = currentProgram.getListing();

        println("========================================================");
        println("HARVESTRIGHT REMOTE-START REACHABILITY (Ghidra)");
        println("========================================================");

        // [1] Dispatcher functions = those referencing the command verbs.
        Set<Function> disp = new HashSet<>();
        for (long sa : CMD_STRING_ADDRS) {
            for (Reference ref : refmgr.getReferencesTo(A(sa))) {
                Function f = fContaining(ref.getFromAddress());
                if (f != null) disp.add(f);
            }
        }
        println("\n[1] Dispatcher function(s) referencing command verbs:");
        for (Function f : disp) {
            println("      " + nm(f) + " @ " + f.getEntryPoint());
        }
        if (disp.isEmpty()) {
            println("      NONE FOUND. Check that auto-analysis finished and");
            println("      the image base is 0x18000.");
        }

        // [2] Serial-reachable code = dispatcher closure + shared handler closure.
        Set<Function> serial = new HashSet<>(disp);
        Set<Function> seen = new HashSet<>();
        for (Function f : new HashSet<>(disp)) callees(f, 8, serial, seen);
        Function sh = fContaining(A(SHARED_HANDLER));
        if (sh != null) {
            serial.add(sh);
            callees(sh, 8, serial, new HashSet<>());
        }
        println("\n[2] Functions reachable from serial dispatch: " + serial.size());

        // [3] SRAM globals written by the shared handler.
        println("\n[3] SRAM globals WRITTEN by shared handler 0x"
                + Long.toHexString(SHARED_HANDLER) + ":");
        TreeSet<Long> handlerGlobals = new TreeSet<>();
        if (sh != null) {
            InstructionIterator it = listing.getInstructions(sh.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                for (Reference ref : ins.getReferencesFrom()) {
                    if (ref.getReferenceType().isWrite()) {
                        long to = ref.getToAddress().getOffset();
                        if (to >= 0x20000000L && to < 0x20080000L) {
                            handlerGlobals.add(to);
                        }
                    }
                }
            }
        }
        for (Long g : handlerGlobals) println("      0x" + Long.toHexString(g));
        if (handlerGlobals.contains(STATE_GLOBAL_GUESS)) {
            println("      -> confirms run-state global 0x"
                    + Long.toHexString(STATE_GLOBAL_GUESS) + " is written here.");
        }

        // [4] For each handler-written global, who else writes it?
        println("\n[4] Cross-reference of handler-written globals:");
        for (Long g : handlerGlobals) {
            Set<Function> writers = new HashSet<>();
            for (Reference ref : refmgr.getReferencesTo(A(g))) {
                if (ref.getReferenceType().isWrite()) {
                    Function wf = fContaining(ref.getFromAddress());
                    if (wf != null) writers.add(wf);
                }
            }
            int serialW = 0;
            for (Function w : writers) if (serial.contains(w)) serialW++;
            println("   global 0x" + Long.toHexString(g) + ": " + writers.size()
                    + " writer-funcs (" + serialW + " serial-reachable, "
                    + (writers.size() - serialW) + " other)");
        }

        // [5] Decompile the shared handler so we can read the logic.
        println("\n[5] Decompiled shared command handler:");
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        if (sh != null) {
            DecompileResults res = di.decompileFunction(sh, 60,
                    new ConsoleTaskMonitor());
            if (res != null && res.getDecompiledFunction() != null) {
                println(res.getDecompiledFunction().getC());
            }
        }

        println("========================================================");
        println("INTERPRETATION:");
        println(" - A handler-written global set from BOTH serial-reachable");
        println("   code AND the run-state machine => remote initiation is");
        println("   POSSIBLE; read the decompiled handler for the exact");
        println("   command + argument gating.");
        println(" - If serial-reachable writes only set status/echo flags and");
        println("   the RUN state is written solely by touchscreen functions,");
        println("   remote START is NOT supported over the wire.");
        println("========================================================");
    }
}
