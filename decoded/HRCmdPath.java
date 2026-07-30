// Find how inbound commands actually reach machine actions.
// Focus: the routing function the dispatcher calls with the command-ID, and the
// handlers for the verbs we could not classify (ADV, ADD, HCS, SPC, GETR, GETP).
//
// Question being answered: does HarvestRight's own app drive DEFROST /
// MORE DRY TIME / CONTINUE through one of these verbs?
//
// @category HarvestRight
// @menupath Tools.HarvestRight.CmdPath

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

public class HRCmdPath extends GhidraScript {

    // Routing function the dispatcher calls after matching a verb + building args.
    final long ROUTER = 0x1b5e4L;
    // String addresses of the unclassified verbs, to find their handlers.
    final String[] VERBS = {"ADV", "ADD", "HCS", "SPC", "GETR", "GETP", "DUTY", "UNIQUE"};

    Address A(long x) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(x);
    }

    void dump(Function f, String label) {
        println("\n================ " + label + " ================");
        if (f == null) { println("(not found)"); return; }
        println("entry: " + f.getEntryPoint() + "  name: " + f.getName());
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        DecompileResults r = di.decompileFunction(f, 90, new ConsoleTaskMonitor());
        if (r != null && r.getDecompiledFunction() != null) {
            String c = r.getDecompiledFunction().getC();
            // keep output manageable
            if (c.length() > 6000) c = c.substring(0, 6000) + "\n/* ...truncated... */";
            println(c);
        }
    }

    @Override
    public void run() throws Exception {
        var fm = currentProgram.getFunctionManager();
        var refmgr = currentProgram.getReferenceManager();

        // 1. The router: what does it do with the command-ID?
        dump(fm.getFunctionContaining(A(ROUTER)), "ROUTER FUN_0001b5e4 (dispatcher -> action)");

        // 2. Find and dump the functions that reference each unclassified verb
        //    string, then whatever they call.
        println("\n\n######## HANDLERS FOR UNCLASSIFIED VERBS ########");
        for (String v : VERBS) {
            // locate the string in memory
            Address found = find(null, v.getBytes());
            while (found != null) {
                // only consider the standalone NUL-terminated table entries
                byte after = currentProgram.getMemory().getByte(found.add(v.length()));
                byte before = currentProgram.getMemory().getByte(found.add(-1));
                if (after == 0 && before == 0) break;
                found = find(found.add(1), v.getBytes());
            }
            if (found == null) { println("\n[" + v + "] string not found"); continue; }
            println("\n[" + v + "] string @ " + found);
            int n = 0;
            for (Reference ref : refmgr.getReferencesTo(found)) {
                Function f = fm.getFunctionContaining(ref.getFromAddress());
                println("   referenced from " + (f == null ? "?" : f.getName())
                        + " @ " + ref.getFromAddress());
                n++;
            }
            if (n == 0) println("   (no code references - matched only via the dispatcher table)");
        }

        println("\n\nDONE. Look at the ROUTER above: if it maps command-IDs to the");
        println("same action objects the touchscreen uses, remote control IS possible");
        println("and the ID range tells us which actions are reachable.");
    }
}
