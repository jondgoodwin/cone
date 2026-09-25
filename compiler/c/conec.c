/** Main program file
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "conec.h"
#include "coneopts.h"
#include "shared/fileio.h"
#include "ir/nametbl.h"
#include "ir/ir.h"
#include "shared/error.h"
#include "shared/timer.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "genllvm/genllvm.h"
#include "ir/incfile.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Run all semantic analysis passes against the AST/IR (after parse and before gen)
void doAnalysis(ConeOptions *opt, ProgramNode **pgm) {

    // Resolve all name uses to their appropriate declaration
    // Note: Some nodes may be replaced (e.g., 'a' to 'self.a')
    NameResState nstate;
    nstate.mod = NULL;
    nstate.typenode = NULL;
    nstate.loopblock = NULL;
    nstate.macromethod = NULL;
    nstate.expander = NULL;
    nstate.scope = 0;
    inodeNameRes(&nstate, (INode**)pgm);
    if (errors) {
        // Name resolution reporting a bad program is one of the two places a
        // phase returns early, so it is one of the two places to check that it
        // left nothing empty behind it
        if (opt->check_tree)
            inodeCheckTree((INode*)*pgm);
        return;
    }

    // Apply syntactic sugar, and perform type inference/check.
    //
    // A second walk of the whole program, not a continuation of the first: name
    // resolution is complete and every name is bound, which is what lets this
    // pass assume a declaration exists wherever one is named.
    //
    // Where the first walk is eager and in source order, this one is
    // demand-driven. Reaching a name analyzes the declaration it names before
    // carrying on, so declarations are analyzed in dependency order and each is
    // analyzed once, however many places reach it. A module iterates its
    // declarations to be sure every one is reached; it does not decide the
    // order. See compiler/c/doc/phases/type-check.md.
    //
    // Along the way:
    // - Macros and generic instantiations are substituted, and the instance is
    //   analyzed as any other declaration would be
    // - Nodes are lowered, injected and replaced, particularly fncall; lowering
    //   is what establishes a node's type, so it belongs to this pass alone
    // - Types fill in infectious information as they are laid out: move
    //   semantics, lifetimes, thread-bound, subtype and inheritance relations
    // - A type reached while it is still being laid out answers what it can --
    //   its identity, but not a size, which is what makes a linked list
    //   expressible and a by-value cycle an error
    // - Data flow analysis runs on each function body as that function's own
    //   type check closes
    // Every field initialised, scope included: blockTypeCheck increments it and
    // clonePushState reads it, so leaving it out is an uninitialised stack read
    // on every compile.
    TypeCheckState tstate;
    tstate.typenode = NULL;
    tstate.fn = NULL;
    tstate.scope = 0;
    inodeTypeCheckAny(&tstate, (INode**)pgm);

    if (opt->check_tree)
        inodeCheckTree((INode*)*pgm);
}

static int writeFile(char *path, char *text, size_t len) {
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return 0;
    size_t written = fwrite(text, 1, len, file);
    return fclose(file) == 0 && written == len;
}

// Write the package's include file, <package>.cone in the output directory
// [Jon 25 Sep, Q3], once it parses and name-resolves as the package's module:
// the self-check, which turns a fault of the generator into a compile error
// here rather than a failure in some importer's build
static void writeIncludeFile(ConeOptions *opt, ProgramNode *pgm, BuildDesc *desc, char *text, size_t len) {
    ModuleNode *root = (ModuleNode*)nodesGet(pgm->modules, 0);
    char *path = fileMakePath(opt->output, &root->namesym->namestr, "cone");
    char *canon = fileCanonicalPath(path);
    if (pgmFindFile(pgm, nametblFind(canon, strlen(canon)))) {
        errorMsg(ErrorIncWrite,
            "Package %s's include file would be written to %s, which is a source file of this compile. Name another output directory.",
            &root->namesym->namestr, path);
        return;
    }

    // Its imports are answered as the root's are: by the root's import lines
    // in a described build, and from the root's own folder otherwise
    char *url = path;
    if (desc == NULL && root->lexer && root->lexer->url) {
        char *rooturl = root->lexer->url;
        size_t folder = fileFolder(rooturl);
        url = memAllocStr(rooturl, folder + strlen(&root->namesym->namestr) + 32);
        url[folder] = '\0';
        strcat(url, &root->namesym->namestr);
        strcat(url, ".include.cone");
    }
    int before = errors;
    ModuleNode *check = parseIncludeCheck(pgm, desc, text, url);
    if (errors == before)
        pgmNameResAlone(pgm, check);
    if (errors != before) {
        char *rejected = fileMakePath(opt->output, &root->namesym->namestr, "cone.rejected");
        writeFile(rejected, text, len);
        errorMsg(ErrorIncCheck,
            "The include file generated for package %s does not parse and name-resolve as %s's module, so it is not written; what was generated is in %s. The generator cannot yet write what this package needs.",
            &root->namesym->namestr, &root->namesym->namestr, rejected);
        return;
    }
    if (!writeFile(path, text, len))
        errorMsg(ErrorIncWrite, "Package %s's include file cannot be written to %s.",
            &root->namesym->namestr, path);
}

int main(int argc, char **argv) {
    ConeOptions coneopt;
    GenState gen;
    int ok;

    // Get compiler's options from passed arguments
    ok = coneOptSet(&coneopt, &argc, argv);
    if (ok <= 0)
        exit(ok == 0 ? 0 : ExitOpts);
    if (argc < 2)
        errorExit(ExitOpts, "Specify a Cone program to compile.");
    coneopt.srcpath = argv[1];
    coneopt.srcname = fileName(coneopt.srcpath);

    // A build description names the package's files and modules and says what
    // to build. Its 'build' line decides whether the output is optimised, which
    // generation's setup reads, so it is read first, and nothing is compiled
    // against a description that could not be read
    parseInit(&coneopt);
    BuildDesc *desc = NULL;
    if (parseIsBuildDesc(coneopt.srcpath)) {
        desc = parseBuildDesc(&coneopt);
        if (errors)
            errorSummary();
    }

    // We set up generation early because we need target info, e.g.: pointer size
    timerBegin(SetupTimer);
    genSetup(&gen, &coneopt);

    // Parse source file, do semantic analysis, and generate code
    timerBegin(ParseTimer);
    ProgramNode* pgmnode = parsePgm(&coneopt, desc);
    // What the parser recorded of where the root's declarations sit, which the
    // include file is copied from, measured before anything depends on it
    if (coneopt.print_spans)
        dclSpanPrintModule((ModuleNode*)nodesGet(pgmnode->modules, 0));
    if (errors == 0) {
        timerBegin(SemTimer);
        doAnalysis(&coneopt, &pgmnode);
        if (errors == 0) {
            timerBegin(GenTimer);
            if (coneopt.print_ir)
                inodePrint(coneopt.output, coneopt.srcname, (INode*)pgmnode);
            // A library's include file is generated from the analysed program,
            // before anything is generated, so that what it cannot declare
            // fails the compile. It is checked and written last, since checking
            // parses and resolves a module beside the program's
            char *inctext = NULL;
            size_t inclen = 0;
            if ((desc && desc->library) || coneopt.emit_include)
                inctext = incFileGenerate(pgmnode, &inclen);
            if (errors == 0) {
                genpgm(&gen, pgmnode);
                genClose(&gen);
                if (inctext)
                    writeIncludeFile(&coneopt, pgmnode, desc, inctext, inclen);
            }
        }
    }
    timerBegin(TimerCount);

    // Close up everything necessary
    if (coneopt.verbosity > 0)
        timerPrint();
    errorSummary();
}
