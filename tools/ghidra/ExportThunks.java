// Export the XBE kernel thunk table: slot address, ordinal, and the import name the
// ghidra-xbe loader assigned. Usage (headless): -postScript ExportThunks.java <thunk_va> <out.csv>
// The ordinal is read from the original file bytes (the loader may rewrite slots in memory).
// @category rejet

import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class ExportThunks extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		Address slot = toAddr(Long.parseLong(args[0].replaceFirst("^0x", ""), 16));
		MemoryBlock b = currentProgram.getMemory().getBlock(slot);
		int n = 0;
		try (PrintWriter w = new PrintWriter(args[1], "UTF-8")) {
			w.println("slot,ordinal,name");
			while (true) {
				long raw = originalInt(slot);
				if (raw == 0) {
					break;
				}
				String name = "";
				for (Reference r : getReferencesFrom(slot)) {
					if (r.getToAddress().isExternalAddress()) {
						Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(r.getToAddress());
						if (s != null) {
							name = s.getName();
						}
					}
				}
				if (name.isEmpty()) {
					Symbol s = getSymbolAt(slot);
					if (s != null && !s.getName().startsWith("DAT_") && !s.getName().startsWith("PTR_")) {
						name = s.getName();
					}
				}
				w.printf("0x%08x,%d,%s%n", slot.getOffset(), raw & 0x7fffffffL, name);
				n++;
				slot = slot.add(4);
				if (!b.contains(slot)) {
					break;
				}
			}
		}
		println("exported " + n + " thunks to " + args[1]);
	}

	private long originalInt(Address a) throws Exception {
		byte[] buf = new byte[4];
		// FileBytes give the pre-relocation, pre-loader-patch bytes when available.
		var fbs = currentProgram.getMemory().getAllFileBytes();
		if (!fbs.isEmpty()) {
			var src = currentProgram.getMemory().getAddressSourceInfo(a);
			if (src != null && src.getFileOffset() >= 0) {
				fbs.get(0).getOriginalBytes(src.getFileOffset(), buf);
				return (buf[0] & 0xffL) | (buf[1] & 0xffL) << 8 | (buf[2] & 0xffL) << 16 | (buf[3] & 0xffL) << 24;
			}
		}
		return getInt(a) & 0xffffffffL;
	}
}
