// Export every call site in the current program to CSV (one row per call instruction target).
// Usage (headless): -postScript ExportCalls.java <out.csv>
//
// Direct calls resolve to the function at the target. Indirect calls through a pointer
// (kernel thunk table, XDK import slots) are followed one level to the function or
// external symbol the pointer names. Calls with no resolvable target (vtable / register
// calls) are written with kind=indirect and an empty callee.
// @category rejet

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class ExportCalls extends GhidraScript {

	private FunctionManager fm;

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		String out = args.length > 0 ? args[0] : askString("Output", "CSV path");
		fm = currentProgram.getFunctionManager();

		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("site,caller,caller_section,callee,callee_name,callee_section,kind");
			InstructionIterator it = currentProgram.getListing().getInstructions(true);
			int n = 0;
			while (it.hasNext() && !monitor.isCancelled()) {
				Instruction ins = it.next();
				if (!ins.getFlowType().isCall()) {
					continue;
				}
				Function caller = fm.getFunctionContaining(ins.getAddress());
				String callerAddr = caller == null ? "" : hex(caller.getEntryPoint());
				String callerSec = section(ins.getAddress());

				boolean wrote = false;
				for (Reference r : ins.getReferencesFrom()) {
					String[] t = resolve(r);
					if (t == null) {
						continue;
					}
					w.printf("%s,%s,%s,%s,%s,%s,%s%n", hex(ins.getAddress()), callerAddr, callerSec,
						t[0], csv(t[1]), t[2], t[3]);
					wrote = true;
					n++;
				}
				if (!wrote) {
					w.printf("%s,%s,%s,,,,indirect%n", hex(ins.getAddress()), callerAddr, callerSec);
					n++;
				}
			}
			println("exported " + n + " call rows to " + out);
		}
	}

	// Returns {address, name, section, kind} or null if the reference is not a call target.
	private String[] resolve(Reference r) {
		Address to = r.getToAddress();
		if (to.isExternalAddress()) {
			Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(to);
			return new String[] { "", s == null ? to.toString() : s.getName(true), "EXTERNAL", "external" };
		}
		if (!to.isMemoryAddress()) {
			return null;
		}
		if (r.getReferenceType().isCall()) {
			Function f = fm.getFunctionAt(to);
			return new String[] { hex(to), f == null ? "" : f.getName(true), section(to), "direct" };
		}
		// call [ptr]: follow the pointer one level.
		Data d = currentProgram.getListing().getDataAt(to);
		if (d != null && d.isPointer() && d.getValue() instanceof Address) {
			Address p = (Address) d.getValue();
			if (p.isExternalAddress()) {
				Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(p);
				return new String[] { hex(to), s == null ? p.toString() : s.getName(true), "EXTERNAL", "pointer" };
			}
			Function f = fm.getFunctionAt(p);
			return new String[] { hex(p), f == null ? "" : f.getName(true), section(p), "pointer" };
		}
		Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(to);
		if (s != null && r.getReferenceType().isRead()) {
			return new String[] { hex(to), s.getName(true), section(to), "pointer" };
		}
		return null;
	}

	private String section(Address a) {
		MemoryBlock b = currentProgram.getMemory().getBlock(a);
		return b == null ? "" : b.getName();
	}

	private static String hex(Address a) {
		return String.format("0x%08x", a.getOffset());
	}

	private static String csv(String s) {
		return s.contains(",") || s.contains("\"") ? "\"" + s.replace("\"", "\"\"") + "\"" : s;
	}
}
