// Decompile the functions containing a list of addresses into one text file.
// Usage (headless): -postScript DecompileList.java <addresses.txt> <out.c>
// addresses.txt: one hex address per line; '#' starts a comment.
// @category rejet

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Paths;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileList extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		int n = 0;
		try (PrintWriter w = new PrintWriter(args[1], "UTF-8")) {
			for (String line : Files.readAllLines(Paths.get(args[0]))) {
				line = line.replaceAll("#.*", "").trim();
				if (line.isEmpty()) {
					continue;
				}
				Address a = toAddr(Long.parseLong(line.replaceFirst("^0x", ""), 16));
				Function f = getFunctionContaining(a);
				if (f == null) {
					w.printf("==== %s: no function%n%n", line);
					continue;
				}
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				w.printf("==== %s %s @ 0x%08x%n", line, f.getName(), f.getEntryPoint().getOffset());
				w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// " + r.getErrorMessage());
				n++;
			}
		}
		finally {
			di.dispose();
		}
		println("decompiled " + n + " functions to " + args[1]);
	}
}
