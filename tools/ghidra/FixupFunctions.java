// Create functions Ghidra's auto-analysis missed (code reached only through vtables /
// function pointers). Run before the export scripts.
// Usage (headless): -postScript FixupFunctions.java
//
// Pass 1: pointers in data sections whose target is an existing instruction that
//         belongs to no function -> create a function there.
// Pass 2: runs of instructions that belong to no function -> create a function at the start
//         of each run (a run starts after a function body, a ret/jmp, or non-code bytes).
// Pass 1 also disassembles a never-disassembled pointer target when a neighbouring slot
// already points at a function (vtable context); pass 2 only touches existing code.
// Repeats until a pass creates nothing.
// @category rejet

import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class FixupFunctions extends GhidraScript {

	private FunctionManager fm;

	// XBE marks most sections executable, so classify by name instead of permissions.
	private static final java.util.Set<String> DATA_BLOCKS =
		java.util.Set.of(".rdata", ".data", ".data1", "XON_RD", "BINKDATA");
	private static final java.util.Set<String> NON_CODE_BLOCKS =
		java.util.Set.of(".rdata", ".data", ".data1", "XON_RD", "BINKDATA", "headers", "$$XTIMAGE", "dspimage", "DOLBY");
	private Listing listing;

	@Override
	protected void run() throws Exception {
		fm = currentProgram.getFunctionManager();
		listing = currentProgram.getListing();
		int before = fm.getFunctionCount();

		int fromPointers = 0, fromRuns = 0, round = 0;
		while (!monitor.isCancelled()) {
			round++;
			int p = pointerPass();
			int r = runPass();
			fromPointers += p;
			fromRuns += r;
			println(String.format("round %d: %d from pointers, %d from orphan runs", round, p, r));
			if (p + r == 0 || round >= 10) {
				break;
			}
		}
		println(String.format("fixup: %d -> %d functions (%d from pointers, %d from orphan runs)",
			before, fm.getFunctionCount(), fromPointers, fromRuns));
	}

	private boolean orphan(Instruction ins) {
		return ins != null && fm.getFunctionContaining(ins.getAddress()) == null;
	}

	private boolean create(Address a) {
		CreateFunctionCmd cmd = new CreateFunctionCmd(a);
		return cmd.applyTo(currentProgram, monitor) && fm.getFunctionAt(a) != null;
	}

	private int pointerPass() throws Exception {
		Memory mem = currentProgram.getMemory();
		int n = 0;
		for (MemoryBlock b : mem.getBlocks()) {
			if (!DATA_BLOCKS.contains(b.getName()) || !b.isInitialized()) {
				continue;
			}
			Address end = b.getEnd();
			for (Address a = b.getStart(); a.compareTo(end) < 0 && !monitor.isCancelled(); a = a.add(4)) {
				if (a.getOffset() + 3 > end.getOffset()) {
					break;
				}
				long v = mem.getInt(a) & 0xffffffffL;
				Address t;
				try {
					t = a.getNewAddress(v);
				}
				catch (Exception e) {
					continue;
				}
				MemoryBlock tb = mem.getBlock(t);
				if (tb == null || NON_CODE_BLOCKS.contains(tb.getName())) {
					continue;
				}
				Instruction ins = listing.getInstructionAt(t);
				if (ins == null && listing.getDefinedDataContaining(t) == null
					&& listing.getInstructionContaining(t) == null && vtableNeighbour(mem, a)) {
					// Never-disassembled target inside a vtable: disassemble it first.
					if (disassemble(t)) {
						ins = listing.getInstructionAt(t);
					}
				}
				if (orphan(ins) && create(t)) {
					n++;
				}
			}
		}
		return n;
	}

	// True if the dword before or after slot points at an existing function entry.
	private boolean vtableNeighbour(Memory mem, Address slot) {
		for (int d : new int[] { -4, 4 }) {
			try {
				Address p = slot.add(d);
				Address t = p.getNewAddress(mem.getInt(p) & 0xffffffffL);
				if (fm.getFunctionAt(t) != null) {
					return true;
				}
			}
			catch (Exception e) {
				// out of range / unmapped: not a neighbour
			}
		}
		return false;
	}

	private int runPass() {
		int n = 0;
		Instruction prev = null;
		InstructionIterator it = listing.getInstructions(true);
		java.util.List<Address> starts = new java.util.ArrayList<>();
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction ins = it.next();
			if (orphan(ins)) {
				boolean start = prev == null
					|| !orphan(prev)
					|| !prev.getMaxAddress().next().equals(ins.getAddress())
					|| !prev.getFlowType().isFallthrough();
				if (start) {
					starts.add(ins.getAddress());
				}
			}
			prev = ins;
		}
		for (Address a : starts) {
			if (monitor.isCancelled()) {
				break;
			}
			// An earlier creation in this pass may already have absorbed this address.
			if (orphan(listing.getInstructionAt(a)) && create(a)) {
				n++;
			}
		}
		return n;
	}
}
