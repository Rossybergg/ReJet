// Export every function in the current program to CSV.
// Usage (headless): -postScript ExportFunctions.java <out.csv>
// @category rejet

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class ExportFunctions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		String out = args.length > 0 ? args[0] : askString("Output", "CSV path");

		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("address,size,section,name,name_source,callers,callees,thunk");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			int n = 0;
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				MemoryBlock b = currentProgram.getMemory().getBlock(f.getEntryPoint());
				SourceType src = f.getSymbol().getSource();
				w.printf("0x%08x,%d,%s,%s,%s,%d,%d,%s%n",
					f.getEntryPoint().getOffset(),
					f.getBody().getNumAddresses(),
					b == null ? "" : b.getName(),
					csv(f.getName(true)),
					src,
					f.getCallingFunctions(monitor).size(),
					f.getCalledFunctions(monitor).size(),
					f.isThunk());
				n++;
			}
			println("exported " + n + " functions to " + out);
		}
	}

	private static String csv(String s) {
		return s.contains(",") || s.contains("\"") ? "\"" + s.replace("\"", "\"\"") + "\"" : s;
	}
}
