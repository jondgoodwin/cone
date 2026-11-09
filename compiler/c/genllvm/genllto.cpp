/** Link-time optimisation: the one place conec uses LLVM's C++ API.
 *
 * LLVM's C API writes bitcode without a module summary, which ThinLTO needs in
 * every module, and has no ThinLTO of its own that is built into the static
 * libraries conec links (its lto.h is only in LTO.dll). So the two pieces
 * --lto needs are here, as plain C functions (genllvm.h):
 *   genlWriteThinBitcode: a module, optimised as a ThinLTO pre-link, written
 *      as bitcode with its summary and hash;
 *   genlThinLink: bitcode files in, one native object out for each.
 * Design: compiler/c/doc/phases/generation.md, "Link-time optimisation".
 *
 * This file is part of the Cone Programming Language. See License.txt.
 */

#include <llvm/Analysis/ModuleSummaryAnalysis.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/LTO/LTO.h>
#include <llvm/LTO/legacy/ThinLTOCodeGenerator.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm-c/Core.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace llvm;

// Write 'mod' to 'path' as bitcode with its ThinLTO summary and module hash:
// what a ThinLTO link reads. Returns 0, or 1 with the reason in 'err'.
extern "C" int genlWriteThinBitcode(LLVMModuleRef mod, const char *path, char *err, size_t errsize) {
    Module &M = *unwrap(mod);
    std::error_code ec;
    raw_fd_ostream out(path, ec, sys::fs::OF_None);
    if (ec) {
        snprintf(err, errsize, "%s: %s", path, ec.message().c_str());
        return 1;
    }
    ProfileSummaryInfo PSI(M);
    ModuleSummaryIndex index = buildModuleSummaryIndex(
        M, [](const Function &) -> BlockFrequencyInfo * { return nullptr; }, &PSI);
    WriteBitcodeToFile(M, out, false, &index, true);
    out.close();
    if (out.has_error()) {
        snprintf(err, errsize, "%s: %s", path, out.error().message().c_str());
        return 1;
    }
    return 0;
}

// The ThinLTO link. 'files' are bitcode files with summaries (the compiles'
// .bc), 'n' of them; the i-th's object is written to <outdir>/<i>.obj ('ext').
// The symbols to keep whatever the modules do with them are 'keep' (a native
// object may call them). A symbol one module defines and another refers to is
// cross-referenced, which ThinLTO must be told: it takes a symbol to be dead
// if no module imports it, though a module may call it out of line.
extern "C" int genlThinLink(const char *outdir, const char *ext, const char **files, int n,
        const char **keep, int nkeep, const char *cpu, const char *features, int pic,
        const char *cachedir, char *err, size_t errsize) {
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmPrinters();
    InitializeAllAsmParsers();

    std::vector<std::unique_ptr<MemoryBuffer>> buffers;
    ThinLTOCodeGenerator gen;
    TargetOptions opts;
    opts.FunctionSections = true;   // so the linker can drop what is unreached (/OPT:REF)
    opts.DataSections = true;
    gen.setTargetOptions(opts);
    gen.setCpu(cpu);
    gen.setAttr(features);
    gen.setCodePICModel(pic ? Reloc::PIC_ : std::optional<Reloc::Model>());
    gen.setCodeGenOptLevel(CodeGenOptLevel::Aggressive);
    gen.setOptLevel(3);
    // A module whose bitcode and imports are the same as the last link's is not
    // generated again: a relink after a change to one package makes that one
    // package's objects, and those of what imports from it
    if (cachedir && *cachedir && !sys::fs::create_directories(cachedir))
        gen.setCacheDir(cachedir);

    std::set<std::string> defined, undefined;
    for (int i = 0; i < n; ++i) {
        auto buf = MemoryBuffer::getFile(files[i]);
        if (!buf) {
            snprintf(err, errsize, "%s: %s", files[i], buf.getError().message().c_str());
            return 1;
        }
        buffers.push_back(std::move(*buf));
        MemoryBufferRef ref(buffers.back()->getBuffer(), files[i]);
        auto input = lto::InputFile::create(ref);
        if (!input) {
            snprintf(err, errsize, "%s: %s", files[i], toString(input.takeError()).c_str());
            return 1;
        }
        for (const lto::InputFile::Symbol &sym : (*input)->symbols()) {
            StringRef name = sym.getName();
            if (sym.isUndefined())
                undefined.insert(name.str());
            else if (!sym.isFormatSpecific()) {
                defined.insert(name.str());
                // A C name is what a C object may call: Cone spells every
                // symbol of its own '_C...' (names-and-namespaces.md)
                if (sym.isGlobal() && !name.starts_with("_C") && !name.starts_with("llvm."))
                    gen.preserveSymbol(name);
            }
        }
        gen.addModule(files[i], buffers.back()->getBuffer());
    }
    for (int i = 0; i < nkeep; ++i)
        gen.preserveSymbol(keep[i]);
    for (const std::string &name : defined)
        if (undefined.count(name))
            gen.crossReferenceSymbol(name);

    gen.run();

    auto &bins = gen.getProducedBinaries();
    if ((int)bins.size() != n) {
        snprintf(err, errsize, "ThinLTO made %d objects from %d modules", (int)bins.size(), n);
        return 1;
    }
    for (int i = 0; i < n; ++i) {
        std::string path = std::string(outdir) + "/" + std::to_string(i) + ext;
        std::error_code ec;
        raw_fd_ostream out(path, ec, sys::fs::OF_None);
        if (ec) {
            snprintf(err, errsize, "%s: %s", path.c_str(), ec.message().c_str());
            return 1;
        }
        out.write(bins[i]->getBufferStart(), bins[i]->getBufferSize());
        out.close();
        if (out.has_error()) {
            snprintf(err, errsize, "%s: %s", path.c_str(), out.error().message().c_str());
            return 1;
        }
    }
    return 0;
}
