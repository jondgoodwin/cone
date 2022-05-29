/** Parser helpers
 * @file
 *
 * - Statement end handling (; and inference)
 * - Block start and end (indented vs. free-flow)
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

// Skip to next statement for error recovery
void parseSkipToNextStmt() {
    // Ensure we are always moving forwards, line by line
    if (lexIsEndOfLine() && !lexIsToken(SemiToken) && !lexIsToken(EofToken) && !lexIsToken(RCurlyToken))
        lexNextToken();
    while (1) {
        // Consume semicolon as end-of-statement
        if (lexIsToken(SemiToken)) {
            lexNextToken();
            return;
        }
        // Treat end-of-line, end-of-file, or '}' as end-of-statement
        // (clearly end-of-line might *not* be end-of-statement)
        if (lexIsEndOfLine() || lexIsToken(EofToken) || lexIsToken(RCurlyToken))
            return;

        lexNextToken();
    }
}

// Is this end-of-statement? if ';', '}', or end-of-file
int parseIsEndOfStatement() {
    return (lex->toktype == SemiToken || lex->toktype == RCurlyToken || lex->toktype == EofToken
        || lexIsStmtBreak());
}

// We expect optional semicolon since statement has run its course
void parseEndOfStatement() {
    // Consume semicolon as end-of-statement signifier, if found
    if (lex->toktype == SemiToken) {
        lexNextToken();
        return;
    }
    // If no semi-colon specified, we expect to be at end-of-line,
    // unless next token is '}' or end-of-file
    if (!lexIsEndOfLine() && lex->toktype != RCurlyToken && lex->toktype != EofToken)
        errorMsgLex(ErrorNoSemi, "Statement finished? Expected semicolon or end of line.");
}

// Return true on '{' or ':'
int parseHasBlock() {
    return (lex->toktype == LCurlyToken || lex->toktype == ColonToken);
}

// Expect a block to start, consume its token and set lexer mode
void parseBlockStart() {
    if (lex->toktype == LCurlyToken) {
        lexNextToken();
        lexBlockStart(FreeFormBlock);
        return;
    }
    else if (lex->toktype == ColonToken) {
        lexNextToken();
        lexBlockStart(lexIsEndOfLine() ? SigIndentBlock : SameStmtBlock);
        return;
    }

    // Generate error and try to recover
    errorMsgLex(ErrorNoLCurly, "Expected ':' or '{' to start a block");
    if (lexIsEndOfLine() && lex->curindent > lex->stmtindent) {
        lexBlockStart(SigIndentBlock);
        return;
    }
    // Skip forward to find something we can use
    while (1) {
        if (lexIsToken(LCurlyToken) || lexIsToken(ColonToken)) {
            parseBlockStart();
            return;
        }
        if (lexIsToken(EofToken))
            break;
        lexNextToken();
    }
}

// Are we at end of block yet? If so, consume token and reset lexer mode
int parseBlockEnd() {
    if (lexIsToken(RCurlyToken) && lex->blkStack[lex->blkStackLvl].blkmode == FreeFormBlock) {
        lexNextToken();
        lexBlockEnd();
        return 1;
    }
    if (lexIsBlockEnd()) {
        lexBlockEnd();
        return 1;
    }
    if (lexIsToken(EofToken)) {
        errorMsgLex(ErrorNoRCurly, "Expected end of block (e.g., '}')");
        return 1;
    }
    return 0;
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
    lexDecrParens();
}
