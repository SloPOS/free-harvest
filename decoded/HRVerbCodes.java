// Extract the verb -> command-ID table from the serial dispatcher, and dump the
// full decompilation to a FILE (the console truncates exactly where the answer
// is).
//
// WHY: every SEND*/SET* verb is referenced only from the dispatcher - there are
// no separate handler functions. The dispatcher compares the input against each
// verb string and, on a match, assigns a small integer command ID. That ID is
// what reaches the shared run-state executor. So the ID table IS the control
// surface, and the tail of the dispatcher shows what the ID is used for.
//
// Output: writes dispatcher_dump.txt next to this script. Paste only the short
// console table; the file carries the bulk.
//
// @category HarvestRight
// @menupath Tools.HarvestRight.VerbCodes

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class HRVerbCodes extends GhidraScript {

    DecompInterface di;
    PrintWriter out;

    // Read a NUL-terminated ASCII string, or null if it does not look like one.
    String readCString(Address a, int max) {
        if (a == null) return null;
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < max; i++) {
                byte b = currentProgram.getMemory().getByte(a.add(i));
                if (b == 0) break;
                if (b < 0x20 || b > 0x7e) return null;
                sb.append((char) b);
            }
        } catch (Exception e) {
            return null;
        }
        return sb.length() == 0 ? null : sb.toString();
    }

    // A wire verb: short, all caps/digits. Filters out format strings and the
    // RAM buffer pointers that also get referenced here.
    boolean looksLikeVerb(String s) {
        if (s == null || s.length() < 2 || s.length() > 14) return false;
        for (char c : s.toCharArray()) {
            if (!(Character.isUpperCase(c) || Character.isDigit(c))) return false;
        }
        return true;
    }

    // The literal pool holds POINTERS to the strings, so resolve both a string
    // at the reference target and a string via the pointer stored there.
    String resolveVerb(Address target) {
        String direct = readCString(target, 16);
        if (looksLikeVerb(direct)) return direct;
        try {
            int p = currentProgram.getMemory().getInt(target);
            Address a = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(p & 0xffffffffL);
            String via = readCString(a, 16);
            if (looksLikeVerb(via)) return via;
        } catch (Exception e) {
            // not a pointer; fine
        }
        return null;
    }

    Address findVerbString(String v) throws Exception {
        Address found = find(null, v.getBytes());
        while (found != null) {
            byte before = currentProgram.getMemory().getByte(found.add(-1));
            byte after = currentProgram.getMemory().getByte(found.add(v.length()));
            if (before == 0 && after == 0) return found;
            found = find(found.add(1), v.getBytes());
        }
        return null;
    }

    void dumpFunc(Function f, String label) {
        out.println();
        out.println("################ " + label + " ################");
        if (f == null) { out.println("(not found)"); return; }
        out.println("entry " + f.getEntryPoint() + "  " + f.getName());
        DecompileResults r = di.decompileFunction(f, 180, new ConsoleTaskMonitor());
        if (r != null && r.getDecompiledFunction() != null) {
            out.println(r.getDecompiledFunction().getC());
        } else {
            out.println("(decompile failed)");
        }
    }

    @Override
    public void run() throws Exception {
        di = new DecompInterface();
        di.openProgram(currentProgram);
        var fm = currentProgram.getFunctionManager();
        var refmgr = currentProgram.getReferenceManager();

        // Locate the dispatcher by a verb it must contain - no hardcoded address.
        Address s = findVerbString("SENDBATCH");
        if (s == null) { println("SENDBATCH string not found - aborting"); return; }
        Function disp = null;
        for (Reference ref : refmgr.getReferencesTo(s)) {
            disp = fm.getFunctionContaining(ref.getFromAddress());
            if (disp != null) break;
        }
        if (disp == null) { println("dispatcher not found - aborting"); return; }

        File f = new File(getSourceFile().getParentFile().getFile(false),
                          "dispatcher_dump.txt");
        out = new PrintWriter(f, "UTF-8");

        println("dispatcher: " + disp.getName() + " @ " + disp.getEntryPoint());
        out.println("dispatcher: " + disp.getName() + " @ " + disp.getEntryPoint());

        // Walk the dispatcher in address order. Each verb string reference is
        // followed, before the next one, by the immediate that becomes its
        // command ID.
        List<String> table = new ArrayList<>();
        Set<String> seen = new LinkedHashSet<>();
        String pending = null;
        Address pendingAt = null;

        InstructionIterator it =
                currentProgram.getListing().getInstructions(disp.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();

            for (Reference r : ins.getReferencesFrom()) {
                String v = resolveVerb(r.getToAddress());
                if (v != null) { pending = v; pendingAt = ins.getAddress(); }
            }

            String m = ins.getMnemonicString().toLowerCase();
            if (pending != null && (m.startsWith("mov") || m.startsWith("cmp"))) {
                for (int i = 0; i < ins.getNumOperands(); i++) {
                    for (Object o : ins.getOpObjects(i)) {
                        if (o instanceof Scalar) {
                            long imm = ((Scalar) o).getUnsignedValue();
                            // Command IDs are small; skip addresses and shifts.
                            if (imm > 0 && imm < 0x100 && m.startsWith("mov")) {
                                String row = String.format(
                                        "  %-12s -> 0x%02X (%3d)   [verb @ %s, imm @ %s]",
                                        pending, imm, imm, pendingAt, ins.getAddress());
                                if (seen.add(pending + "#" + imm)) table.add(row);
                                pending = null;
                            }
                        }
                    }
                }
            }
        }

        println("");
        println("==== VERB -> COMMAND ID ====");
        out.println("");
        out.println("==== VERB -> COMMAND ID ====");
        for (String row : table) { println(row); out.println(row); }
        println("(" + table.size() + " pairs)");
        out.println("(" + table.size() + " pairs)");

        // Full decompiles, unbounded, into the file.
        dumpFunc(disp, "SERIAL DISPATCHER (full)");

        // Everything the dispatcher calls - the executor is in here.
        out.println();
        out.println("################ DISPATCHER CALLEES ################");
        Set<Function> callees = new LinkedHashSet<>(disp.getCalledFunctions(
                new ConsoleTaskMonitor()));
        for (Function c : callees) {
            out.println("  calls " + c.getName() + " @ " + c.getEntryPoint());
        }
        for (Function c : callees) {
            dumpFunc(c, "CALLEE " + c.getName());
        }

        out.flush();
        out.close();
        println("");
        println("FULL DUMP WRITTEN TO: " + f.getAbsolutePath());
        println("Paste only the table above; the file has the rest.");
    }
}
