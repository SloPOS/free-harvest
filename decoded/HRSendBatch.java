// Find the payload format of the SEND* command family.
//
// WHY: live testing showed ADV can start a cycle but improperly - it reaches
// "load trays" and the compressor never engages. FUN_00021414 (the shared
// run-state handler) is fed by both the panel and the serial path, so the
// working start differs from raw ADV only in the SEQUENCE of transitions and
// the batch PARAMETERS set up first. SENDBATCH/SENDCANDY/SENDCUSTOM are the
// verbs that carry those parameters. This script decompiles their handlers and
// the dispatcher, so we can read the exact wire payload and reconstruct a
// proper start.
//
// Robust to the address shift between builds: everything is located by string
// search + xref, nothing is hardcoded.
//
// @category HarvestRight
// @menupath Tools.HarvestRight.SendBatch

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.util.LinkedHashSet;
import java.util.Set;

public class HRSendBatch extends GhidraScript {

    // The data-push family, plus the config setters that a start likely needs
    // to have run first (recipe / mode / date).
    final String[] VERBS = {
        "SENDBATCH", "SENDCANDY", "SENDCUSTOM", "SENDSCIENCE",
        "SETPREF", "SETBNAME", "SETDATE"
    };

    DecompInterface di;

    Address A(long x) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(x);
    }

    // A standalone NUL-terminated table entry, not the verb as a substring of a
    // longer string (SENDBATCH would otherwise match inside a format string).
    Address findVerbString(String v) throws Exception {
        Address found = find(null, v.getBytes());
        while (found != null) {
            byte before = currentProgram.getMemory().getByte(found.add(-1));
            byte after  = currentProgram.getMemory().getByte(found.add(v.length()));
            if (before == 0 && after == 0) return found;
            found = find(found.add(1), v.getBytes());
        }
        return null;
    }

    void dump(Function f, String label) {
        println("\n======== " + label + " ========");
        if (f == null) { println("(not found)"); return; }
        println("entry " + f.getEntryPoint() + "  " + f.getName());
        DecompileResults r = di.decompileFunction(f, 120, new ConsoleTaskMonitor());
        if (r != null && r.getDecompiledFunction() != null) {
            String c = r.getDecompiledFunction().getC();
            if (c.length() > 9000) c = c.substring(0, 9000) + "\n/* ...truncated... */";
            println(c);
        } else {
            println("(decompile failed)");
        }
    }

    @Override
    public void run() throws Exception {
        di = new DecompInterface();
        di.openProgram(currentProgram);
        var fm = currentProgram.getFunctionManager();
        var refmgr = currentProgram.getReferenceManager();

        Set<Function> handlers = new LinkedHashSet<>();

        for (String v : VERBS) {
            Address s = findVerbString(v);
            if (s == null) { println("[" + v + "] string not found"); continue; }
            println("[" + v + "] @ " + s);
            for (Reference ref : refmgr.getReferencesTo(s)) {
                Function f = fm.getFunctionContaining(ref.getFromAddress());
                println("    ref from " + (f == null ? "?" : f.getName())
                        + " @ " + ref.getFromAddress());
                // The dispatcher references every verb string; its handler is
                // elsewhere. Collect referencing functions that are NOT the
                // giant strcmp dispatcher (heuristic: dispatcher references
                // MANY verbs). We decompile all of them below and let the
                // reader judge.
                if (f != null) handlers.add(f);
            }
        }

        // Decompile each referencing function once. For SEND* the interesting
        // one parses the argument buffer into a recipe/batch struct - look for
        // strtol/atoi-like field extraction and stores into a contiguous
        // struct, then a call into the run-state handler.
        println("\n\n#### DECOMPILED REFERENCING FUNCTIONS ####");
        for (Function f : handlers) {
            dump(f, f.getName());
        }

        println("\n\nWHAT TO LOOK FOR:");
        println(" - In a SEND* handler: how many fields it reads from the arg");
        println("   buffer and where it stores them = the wire payload format.");
        println(" - A call from that handler into the run-state updater with a");
        println("   'start' code = the proper start path ADV skips.");
    }
}
