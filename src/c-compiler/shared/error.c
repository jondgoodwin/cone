/** Error Handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "error.h"
#include "timer.h"
#include "../parser/lexer.h"
#include "../ir/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int errors = 0;
int warnings = 0;

// Send an error message to stderr
void errorExit(int exitcode, const char *msg, ...) {
    // Do a formatted output, passing along all args
    va_list argptr;
    va_start(argptr, msg);
    vfprintf(stderr, msg, argptr);
    va_end(argptr);
    fputs("\n", stderr);

    // Exit with return code.
    // Nothing waits on stdin here: a Debug build that paused for a keystroke
    // would block every test case until its timeout.
    exit(exitcode);
}

// Send an error message to stderr
void errorOut(int code, const char *msg, va_list args) {
    // Prefix for error message
    if (code < WarnCode) {
        errors++;
        fprintf(stderr, "Error %d: ", code);
    }
    else if (code < Uncounted) {
        warnings++;
        fprintf(stderr, "Warning %d: ", code);
    }

    // Do a formatted output of message, passing along all args
    vfprintf(stderr, msg, args);
    fputs("\n", stderr);
}

// Send an error message plus code context to stderr
void errorOutCode(char *tokp, uint32_t linenbr, char *linep, char *url, int code, const char *msg, va_list args) {
    char *srcp;
    int pos;

    // Send out the error message and count
    errorOut(code, msg, args);

    // Reflect the source code line
    fputs(" --> ", stderr);
    srcp = linep;
    while (*srcp && *srcp!='\n')
        fputc(*srcp++, stderr);
    fputc('\n', stderr);

    // Depict where error message applies along with source file/pos info.
    // Pad and count one column per character, not per byte: a UTF-8
    // continuation byte (10xxxxxx) belongs to the character before it, so
    // counting it would push the caret right of what it points at.
    fprintf(stderr, "     ");
    pos = 1;
    for (srcp = linep; srcp < tokp; ++srcp) {
        if ((*srcp & 0xC0) == 0x80)
            continue;
        fputc(*srcp == '\t' ? '\t' : ' ', stderr);
        ++pos;
    }
    fprintf(stderr, "^--- %s:%d:%d\n", url, linenbr, pos);
}

// How many frames of an instantiation trace are worth printing.
//
// A node cloned by a generic instantiation or a macro expansion remembers what
// expanded it, and that chain is as long as the expansion nested. Ordinary code
// nests one or two deep, so the whole chain is worth showing. An expansion that
// never terminates nests until ErrorInstDepth stops it, and printing all of it
// would bury the diagnostic itself under hundreds of identical frames.
#define ErrorInstTraceMax 4

// One frame of an instantiation trace, without following the chain further
static void errorMsgFrame(INode *node, int code, const char *msg, ...) {
    va_list argptr;
    va_start(argptr, msg);
    errorOutCode(node->srcp, node->linenbr, node->linep, node->lexer->url, code, msg, argptr);
    va_end(argptr);
}

// Send an error message to stderr
void errorMsgNode(INode *node, int code, const char *msg, ...) {
    va_list argptr;
    va_start(argptr, msg);
    errorOutCode(node->srcp, node->linenbr, node->linep, node->lexer->url, code, msg, argptr);
    va_end(argptr);

    // Name what instantiated this node, and what instantiated that, outward
    uint32_t shown = 0;
    for (INode *instnode = node->instnode; instnode; instnode = instnode->instnode) {
        if (shown++ == ErrorInstTraceMax) {
            uint32_t more = 0;
            for (INode *rest = instnode; rest; rest = rest->instnode)
                ++more;
            errorMsg(Uncounted, "... and %u further instantiation%s not shown", more, more == 1 ? "" : "s");
            break;
        }
        errorMsgFrame(instnode, Uncounted, "... as instantiated by this part of the source code");
    }
}

// Send an error message to stderr
void errorMsgLex(int code, const char *msg, ...) {
    va_list argptr;
    va_start(argptr, msg);
    errorOutCode(lex->tokp, lex->linenbr, lex->linep, lex->url, code, msg, argptr);
    va_end(argptr);
}

// Send an error message to stderr
void errorMsg(int code, const char *msg, ...) {
    va_list argptr;
    va_start(argptr, msg);
    errorOut(code, msg, argptr);
    va_end(argptr);
}

// Report a state the compiler had established cannot happen, and stop.
//
// This is what an 'assert(0 && "unreachable")' was doing in a Debug build and
// what it was not doing in a Release one: NDEBUG compiles the assert to
// ((void)0), so the shipping compiler carried on with a node it had just said
// was impossible -- reading it as whatever the next 'case' expected, or running
// the rest of the function on a value it could not have. That is a wrong object
// file or an access violation, either way with nothing said.
//
// It reports through errorMsgNode rather than errorExit alone so the author
// gets a source position, including the instantiation trace: a defect that only
// shows up inside a generic expansion is nearly unreportable without the code
// that expanded it. Then it exits, because the compiler has just established
// that its own invariants do not hold and anything further it produced would be
// guesswork. ExitGen is that exit: it already means "stopped, internally", and
// splitting a second code off it would enlarge the driver contract for a status
// no scenario can produce.
//
// Nothing calls this for a bad program. A condition a source can reach is a
// missing diagnostic and gets a code and a message of its own.
void errorUnreachable(INode *node, const char *msg) {
    errorMsgNode(node, ErrorUnreachable,
        "Compiler defect: %s. Please report this, with the source that provoked it.", msg);
    errorExit(ExitGen, "Compile abandoned: the compiler reached a state it had ruled out");
}

// Generate final message for a compile
void errorSummary() {
    if (errors > 0)
        errorExit(ExitError, "Unsuccessful compile: %d errors, %d warnings", errors, warnings);
    fprintf(stderr, "Compile finished in %.6g sec (%lu kb). %d warnings detected\n", timerSummary(), memUsed()/1024, warnings);
}
