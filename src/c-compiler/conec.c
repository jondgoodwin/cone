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

#include <stdio.h>
#include <assert.h>

// Run all semantic analysis passes against the AST/IR (after parse and before gen)
void doAnalysis(ConeOptions *opt, ProgramNode **pgm) {

    // Resolve all name uses to their appropriate declaration
    // Note: Some nodes may be replaced (e.g., 'a' to 'self.a')
    NameResState nstate;
    nstate.mod = NULL;
    nstate.typenode = NULL;
    nstate.loopblock = NULL;
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
    // order. See design/phases/type-check.md.
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

    // We set up generation early because we need target info, e.g.: pointer size
    timerBegin(SetupTimer);
    genSetup(&gen, &coneopt);

    // Parse source file, do semantic analysis, and generate code
    timerBegin(ParseTimer);
    ProgramNode* pgmnode = parsePgm(&coneopt);
    if (errors == 0) {
        timerBegin(SemTimer);
        doAnalysis(&coneopt, &pgmnode);
        if (errors == 0) {
            timerBegin(GenTimer);
            if (coneopt.print_ir)
                inodePrint(coneopt.output, coneopt.srcname, (INode*)pgmnode);
            genpgm(&gen, pgmnode);
            genClose(&gen);
        }
    }
    timerBegin(TimerCount);

    // Close up everything necessary
    if (coneopt.verbosity > 0)
        timerPrint();
    errorSummary();
}
