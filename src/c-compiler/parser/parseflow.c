/** Parse executable statements and blocks
 * @file
 *
 * The parser translates the lexer's tokens into IR nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../ir/nametbl.h"
#include "lexer.h"

#include <stdio.h>

INode *parseEach(ParseState *parse, INode *blk);

// Parse control flow suffixes
INode *parseFlowSuffix(ParseState *parse, INode *node) {
    // Translate 'this' block sugar to declaring 'this' at start of block
    if (lexIsToken(LCurlyToken)) {
        INode *var = (INode*)newVarDclFull(thisName, VarDclTag, voidType, (INode*)immPerm, node);
        BlockNode *blk = (BlockNode*)parseBlock(parse);
        nodesInsert(&blk->stmts, var, 0);
        return (INode *)blk;
    }
    while (lexIsToken(IfToken) || lexIsToken(WhileToken) || lexIsToken(EachToken)) {
        if (lexIsToken(IfToken)) {
            BlockNode *blk;
            IfNode *ifnode = newIfNode();
            lexNextToken();
            nodesAdd(&ifnode->condblk, parseAnyExpr(parse));
            nodesAdd(&ifnode->condblk, (INode*)(blk = newBlockNode()));
            nodesAdd(&blk->stmts, node);
            node = (INode*)ifnode;
        }
        else if (lexIsToken(WhileToken)) {
            BlockNode *blk;
            WhileNode *wnode = newWhileNode();
            lexNextToken();
            wnode->condexp = parseAnyExpr(parse);
            wnode->blk = (INode*)(blk = newBlockNode());
            nodesAdd(&blk->stmts, node);
            node = (INode*)wnode;
        }
        else if (lexIsToken(EachToken)) {
            BlockNode *blk = newBlockNode();
            nodesAdd(&blk->stmts, node);
            node = parseEach(parse, (INode *)blk);
        }
    }
    parseSemi();
    return node;
}

// Parse an expression statement within a function
INode *parseExpStmt(ParseState *parse) {
    return parseFlowSuffix(parse, (INode *)parseAnyExpr(parse));
}

// Parse a return statement
INode *parseReturn(ParseState *parse) {
    ReturnNode *stmtnode = newReturnNode();
    lexNextToken(); // Skip past 'return'
    if (!lexIsToken(SemiToken))
        stmtnode->exp = parseAnyExpr(parse);
    return parseFlowSuffix(parse, (INode*)stmtnode);
}

// Parse if statement/expression
INode *parseIf(ParseState *parse) {
    IfNode *ifnode = newIfNode();
    lexNextToken();
    nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
    nodesAdd(&ifnode->condblk, parseBlock(parse));
    while (1) {
        // Process final else clause and break loop
        // Note: this code makes "else if" equivalent to "elif"
        if (lexIsToken(ElseToken)) {
            lexNextToken();
            if (!lexIsToken(IfToken)) {
                nodesAdd(&ifnode->condblk, voidType); // else distinguished by a 'void' condition
                nodesAdd(&ifnode->condblk, parseBlock(parse));
                break;
            }
        }
        else if (!lexIsToken(ElifToken))
            break;

        // Elif processing
        lexNextToken();
        nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
        nodesAdd(&ifnode->condblk, parseBlock(parse));
    }
    return (INode *)ifnode;
}

// Parse loop block
INode *parseLoop(ParseState *parse) {
    LoopNode *wnode = newLoopNode();
    lexNextToken();
    wnode->blk = parseBlock(parse);
    return (INode *)wnode;
}

// Parse while block
INode *parseWhile(ParseState *parse) {
    WhileNode *wnode = newWhileNode();
    lexNextToken();
    wnode->condexp = parseSimpleExpr(parse);
    wnode->blk = parseBlock(parse);
    return (INode *)wnode;
}

// Parse each block
INode *parseEach(ParseState *parse, INode *blk) {
    BlockNode *bnode = newBlockNode();
    WhileNode *wnode = newWhileNode();

    // Obtain all the parsed pieces
    lexNextToken();
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoVar, "Missing variable name");
        return (INode *)bnode;
    }
    Name* elemname = lex->val.ident;
    lexNextToken();
    if (!lexIsToken(InToken)) {
        errorMsgLex(ErrorBadTok, "Missing 'in'");
        return (INode *)bnode;
    }
    lexNextToken();
    INode *iter = parseSimpleExpr(parse);
    INode *step = NULL;
    int isrange = 0;
    if (iter->tag == FnCallTag && ((FnCallNode*)iter)->methprop) {
        Name *methodnm = ((NameUseNode*)((FnCallNode*)iter)->methprop)->namesym;
        if (methodnm == leName || methodnm == ltName)
            isrange = 1;
        else if (methodnm == geName || methodnm == gtName)
            isrange = -1;
    }
    if (isrange && lexIsToken(ByToken)) {
        lexNextToken();
        step = parseSimpleExpr(parse);
    }
    if (blk == NULL)
        blk = parseBlock(parse);
    wnode->blk = blk;

    // Assemble logic for a range (with optional step), e.g.:
    // { mut elemname = initial; while elemname <= iterend { ... ; elemname += step}}
    if (isrange) {
        FnCallNode *itercmp = (FnCallNode *)iter;
        VarDclNode *elemdcl = newVarDclNode(elemname, VarDclTag, (INode*)mutPerm);
        elemdcl->value = itercmp->objfn;
        nodesAdd(&((BlockNode*)bnode)->stmts, (INode*)elemdcl);
        itercmp->objfn = (INode*)newNameUseNode(elemname);
        if (step) {
            FnCallNode *pluseq = newFnCallOpname((INode*)newNameUseNode(elemname), plusEqName, 1);
            pluseq->flags |= FlagLvalOp;
            nodesAdd(&pluseq->args, step);
            nodesAdd(&((BlockNode*)wnode->blk)->stmts, (INode*)pluseq);
        }
        else {
            INode *incr = (INode *)newFnCallOpname((INode *)newNameUseNode(elemname), isrange > 0 ? incrPostName : decrPostName, 0);
            nodesAdd(&((BlockNode*)wnode->blk)->stmts, incr);
        }
        wnode->condexp = iter;
        nodesAdd(&bnode->stmts, (INode*)wnode);
    }
    return (INode *)bnode;
}

// Parse a block of statements/expressions
INode *parseBlock(ParseState *parse) {
    BlockNode *blk = newBlockNode();

    if (!lexIsToken(LCurlyToken))
        parseLCurly();
    if (!lexIsToken(LCurlyToken))
        return (INode*)blk;
    lexNextToken();

    blk->stmts = newNodes(8);
    while (! lexIsToken(EofToken) && ! lexIsToken(RCurlyToken)) {
        switch (lex->toktype) {
        case SemiToken:
            lexNextToken();
            break;

        case RetToken:
            nodesAdd(&blk->stmts, parseReturn(parse));
            break;

        case IfToken:
            nodesAdd(&blk->stmts, parseIf(parse));
            break;

        case LoopToken:
            nodesAdd(&blk->stmts, parseLoop(parse));
            break;

        case WhileToken:
            nodesAdd(&blk->stmts, parseWhile(parse));
            break;

        case EachToken:
            nodesAdd(&blk->stmts, parseEach(parse, NULL));
            break;

        case BreakToken:
        {
            INode *node = (INode*)newBreakNode();
            lexNextToken();
            nodesAdd(&blk->stmts, parseFlowSuffix(parse, node));
            break;
        }

        case ContinueToken:
        {
            INode *node = (INode*)newContinueNode();
            lexNextToken();
            nodesAdd(&blk->stmts, parseFlowSuffix(parse, node));
            break;
        }

        case DoToken:
            lexNextToken();
            nodesAdd(&blk->stmts, parseBlock(parse));
            break;

        case LCurlyToken:
            nodesAdd(&blk->stmts, parseBlock(parse));
            break;

        // A local variable declaration, if it begins with a permission
        case PermToken:
            nodesAdd(&blk->stmts, (INode*)parseVarDcl(parse, immPerm, ParseMayConst|ParseMaySig|ParseMayImpl));
            parseSemi();
            break;

        default:
            nodesAdd(&blk->stmts, parseExpStmt(parse));
        }
    }

    parseRCurly();
    return (INode*)blk;
}
