/** Parser helpers
 * @file
 *
 * - Statement end handling
 * - Block start and end
 * - Closing paren vs. bracket
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../shared/fileio.h"
#include "../ir/nametbl.h"
#include "../coneopts.h"
#include "lexer.h"

#include <stdio.h>
#include <string.h>

// Skip to next statement for error recovery: consume through the next ';',
// or stop short of a '}' or end-of-file, which the enclosing block handles
void parseSkipToNextStmt() {
    while (1) {
        if (lexIsToken(SemiToken)) {
            lexNextToken();
            return;
        }
        if (lexIsToken(EofToken) || lexIsToken(RCurlyToken))
            return;
        lexNextToken();
    }
}

// Is this end-of-statement? if ';', '}', or end-of-file
int parseIsEndOfStatement() {
    return lexIsToken(SemiToken) || lexIsToken(RCurlyToken) || lexIsToken(EofToken);
}

// Require the ';' that ends every statement not ending in a block.
// Nothing stands in for it: not the end of a line, not the '}' that closes the
// block, not the end of the file. The diagnostic is placed after the token the
// ';' should follow, which is where the fix goes.
void parseEndOfStatement() {
    if (lexIsToken(SemiToken)) {
        lexNextToken();
        return;
    }
    errorMsgLexAfter(ErrorNoSemi, "Expected ';' to end the statement");
}

// Return true if a block starts here. ':' counts so that the off-side form
// reaches parseBlockStart and is diagnosed as what it is, rather than falling
// through to whatever a construct does when it has no block.
int parseHasBlock() {
    return lexIsToken(LCurlyToken) || lexIsToken(ColonToken);
}

// Expect '{' and consume it
void parseBlockStart() {
    if (lexIsToken(LCurlyToken)) {
        lexNextToken();
        return;
    }
    if (lexIsToken(ColonToken)) {
        errorMsgLex(ErrorColonBlock, "A block starts with '{', not ':'. Indentation does not delimit a block: write '{' here and '}' after the block's last statement");
        lexNextToken();
        return;
    }
    errorMsgLex(ErrorNoLCurly, "Expected '{' to start a block");
    // Recover by skipping forward to the '{' the block was meant to have
    while (!lexIsToken(LCurlyToken)) {
        if (lexIsToken(EofToken))
            return;
        lexNextToken();
    }
    lexNextToken();
}

// Are we at end of block yet? If so, consume '}'
int parseBlockEnd() {
    if (lexIsToken(RCurlyToken)) {
        lexNextToken();
        return 1;
    }
    if (lexIsToken(EofToken)) {
        errorMsgLex(ErrorNoRCurly, "Expected end of block (e.g., '}')");
        return 1;
    }
    return 0;
}

// Record the span of the statement just parsed (dclspan.h). The lexer is on the
// token after it, so the span ends where the previous token did
DclSpan *parseSpan(ParseState *parse, DclSpans **listp, INode *node, char *start, char *kw, uint16_t kind) {
    DclSpan *span = dclSpanAdd(listp, node, lex, start, kw, lex->prevend, kind);
    if (node && (node->tag == FnDclTag || node->tag == VarDclTag)) {
        span->body = parse->bodyp;
        span->bodyend = parse->bodyendp;
        if (node->tag == VarDclTag) {
            span->nameend = parse->nameendp;
            span->typed = (uint16_t)parse->typed;
        }
    }
    return span;
}

// Expect closing token (e.g., right parenthesis). If not found, search for it or '}' or ';'
void parseCloseTok(uint16_t closetok) {
    if (!lexIsToken(closetok))
        errorMsgLex(ErrorNoRParen, "Expected right parenthesis - skipping forward to find it");
    while (!lexIsToken(closetok)) {
        if (lexIsToken(EofToken) || lexIsToken(SemiToken) || lexIsToken(RCurlyToken))
            return;
        lexNextToken();
    }
    lexNextToken();
}
