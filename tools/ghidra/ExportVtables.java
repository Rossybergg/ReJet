// Export candidate vtables: runs of >= 2 aligned pointers to function entries in
// data sections, whose first slot is referenced from code (the constructor /
// destructor storing the vtable pointer).
// Usage (headless): -postScript ExportVtables.java <out.csv>
// Columns: vtable, slots, writers (functions referencing the vtable, ';'-separated),
//          slot targets (';'-separated, function entry addresses).
// @category rejet

import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class ExportVtables extends GhidraScript {

	// XBE marks most sections executable, so classify by name instead of permissions.
	private static final java.util.Set<String> DATA_BLOCKS =
		java.util.Set.of(".rdata", ".data", ".data1", "XON_RD", "BINKDATA");

	@Override
	protected void run() throws Exception {
		String out = getScriptArgs()[0];
		Memory mem = currentProgram.getMemory();
		FunctionManager fm = currentProgram.getFunctionManager();
		int n = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("vtable,slots,writers,targets");
			for (MemoryBlock b : mem.getBlocks()) {
				if (!DATA_BLOCKS.contains(b.getName()) || !b.isInitialized()) {
					continue;
				}
				long a = (b.getStart().getOffset() + 3) & ~3L;
				long end = b.getEnd().getOffset() - 3;
				while (a <= end && !monitor.isCancelled()) {
					List<String> targets = new ArrayList<>();
					long p = a;
					while (p <= end) {
						Function f = fm.getFunctionAt(toAddr(mem.getInt(toAddr(p)) & 0xffffffffL));
						if (f == null || f.isExternal()) {
							break;
						}
						// A new code-referenced table inside the run starts a new vtable.
						if (p != a && codeRefs(toAddr(p)).size() > 0) {
							break;
						}
						targets.add(String.format("0x%08x", f.getEntryPoint().getOffset()));
						p += 4;
					}
					TreeSet<String> writers = codeRefs(toAddr(a));
					if (targets.size() >= 2 && !writers.isEmpty()) {
						w.printf("0x%08x,%d,%s,%s%n", a, targets.size(), String.join(";", writers),
							String.join(";", targets));
						n++;
						a = p;
					}
					else {
						a += 4;
					}
				}
			}
		}
		println("exported " + n + " vtables to " + out);
	}

	private TreeSet<String> codeRefs(Address a) {
		TreeSet<String> s = new TreeSet<>();
		for (Reference r : getReferencesTo(a)) {
			Function f = getFunctionContaining(r.getFromAddress());
			if (f != null) {
				s.add(String.format("0x%08x", f.getEntryPoint().getOffset()));
			}
		}
		return s;
	}
}
