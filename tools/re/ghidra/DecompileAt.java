// Headless Ghidra script: decompile functions at the given addresses.
// Args: <out_file> <name:hexaddr[:skip]> [...]   or   <out_file> @<file with one arg per line>
// Used by tools/re/decompile.py.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileAt extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 2 && args[1].startsWith("@")) {
            java.util.List<String> lines = java.nio.file.Files.readAllLines(java.nio.file.Paths.get(args[1].substring(1)));
            lines.add(0, args[0]);
            args = lines.toArray(new String[0]);
        }
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        // First pass: create + name every function so calls between them read well.
        for (int i = 1; i < args.length; i++) {
            String[] kv = args[i].split(":");
            Address a = toAddr(Long.parseLong(kv[1], 16) + currentProgram.getImageBase().getOffset());
            Function f = getFunctionAt(a);
            if (f == null) {
                disassemble(a);
                f = createFunction(a, kv[0]);
            }
            if (f != null && !kv[0].startsWith("FUN_"))
                f.setName(kv[0], SourceType.USER_DEFINED);
        }
        for (int i = 1; i < args.length; i++) {
            String[] kv = args[i].split(":");
            if (kv.length > 2 && kv[2].equals("skip"))
                continue;
            Address a = toAddr(Long.parseLong(kv[1], 16) + currentProgram.getImageBase().getOffset());
            Function f = getFunctionAt(a);
            out.println("// ===== " + kv[0] + " @ 0x" + kv[1]);
            if (f == null) {
                out.println("// (no function)");
                continue;
            }
            DecompileResults r = ifc.decompileFunction(f, 120, monitor);
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
        }
        out.close();
    }
}
