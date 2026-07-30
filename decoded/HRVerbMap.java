// Final: map each serial verb string -> the command-ID the dispatcher assigns,
// so we can see exactly which actions are reachable over the wire.
//
// The dispatcher FUN_000291b0 does: strcmp(input, VERB_i) then sets uVar16=ID.
// The verb strings are at DAT_000296ac.. (pointers). We read those pointers,
// read the target strings, and pair them with the IDs in dispatch order.
//
// @category HarvestRight
// @menupath Tools.HarvestRight.VerbMap

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryAccessException;

public class HRVerbMap extends GhidraScript {

    // Pointer table of command-verb strings, in dispatch order, from the
    // decompiled dispatcher (DAT_000296ac ... DAT_00029720) plus the earlier
    // block (DAT_00029448 ... DAT_0002949c). We print all we can read.
    final long PTRTABLE_1_START = 0x29448L;   // first block of verb ptrs
    final long PTRTABLE_1_END   = 0x2949cL;
    final long PTRTABLE_2_START = 0x296acL;   // second block (STATUS..SETDATE..)
    final long PTRTABLE_2_END   = 0x29720L;

    Address A(long x) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(x);
    }

    String readCStr(long ptr) {
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 32; i++) {
                byte b = currentProgram.getMemory().getByte(A(ptr + i));
                if (b == 0) break;
                sb.append((char) (b & 0xff));
            }
        } catch (MemoryAccessException e) {
            return "<unreadable>";
        }
        return sb.toString();
    }

    void dumpTable(long start, long end, String label) throws Exception {
        println("\n=== " + label + " (verb pointer table 0x"
                + Long.toHexString(start) + "..0x" + Long.toHexString(end) + ") ===");
        int idx = 0;
        for (long p = start; p <= end; p += 4) {
            long ptr = currentProgram.getMemory().getInt(A(p)) & 0xffffffffL;
            String s = readCStr(ptr);
            println(String.format("   [%2d] ptr 0x%08x -> \"%s\"", idx, ptr, s));
            idx++;
        }
    }

    @Override
    public void run() throws Exception {
        println("SERIAL-REACHABLE COMMAND VERBS (what the wire protocol accepts):");
        dumpTable(PTRTABLE_1_START, PTRTABLE_1_END, "Block 1");
        dumpTable(PTRTABLE_2_START, PTRTABLE_2_END, "Block 2");
        println("\nThese are the ONLY verbs the serial dispatcher matches. If none");
        println("of them is a 'start cycle' verb, remote cycle-start is not exposed");
        println("over the wire (the touchscreen Start uses a different entry point).");
    }
}
