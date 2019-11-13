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

INode *parseEach(ParseState *parse, INode *blk, LifetimeNode *life);

// This helper routine inserts 'break if !condexp' at beginning of block
void parseInsertWhileBreak(INode *blk, INode *condexp) {
    INode *breaknode = (INode*)newBreakNode();
    BlockNode *ifblk = newBlockNode();
    nodesAdd(&ifblk->stmts, breaknode);
    LogicNode *notiter = newLogicNode(NotLogicTag);
    notiter->lexp = condexp;
    IfNode *ifnode = newIfNode();
    nodesAdd(&ifnode->condblk, (INode *)notiter);
    nodesAdd(&ifnode->condblk, (INode *)ifblk);
    nodesInsert(&((BlockNode*)blk)->stmts, (INode*)ifnode, 0);
}

// Parse control flow suffixes
INode *parseFlowSuffix(ParseState *parse, INode *node) {
    // Translate 'this' block sugar to declaring 'this' at start of block
    if (lexIsToken(LCurlyToken)) {
        INode *var = (INode*)newVarDclFull(thisName, VarDclTag, NULL, (INode*)immPerm, node);
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
            LoopNode *loopnode = newLoopNode();
            BlockNode *blk = newBlockNode();
            loopnode->blk = (INode*)blk;
            lexNextToken();
            parseInsertWhileBreak((INode*)blk, parseAnyExpr(parse));
            nodesAdd(&blk->stmts, node);
            node = (INode*)loopnode;
        }
        else if (lexIsToken(EachToken)) {
            BlockNode *blk = newBlockNode();
            nodesAdd(&blk->stmts, node);
            node = parseEach(parse, (INode *)blk, NULL);
        }
    }
    parseEndOfStatement();
    return node;
}

// Parse an expression statement within a function
INode *parseExpStmt(ParseState *parse) {
    return parseFlowSuffix(parse, (INode *)parseAnyExpr(parse));
}

// Parse a block or an expression statement followed by semicolon
INode *parseBlockOrStmt(ParseState *parse) {
    if (lexIsToken(RCurlyToken)) {
        return parseBlock(parse);
    }
    BlockNode *blk = newBlockNode();
    nodesAdd(&blk->stmts, parseExpStmt(parse));
    parseEndOfStatement();
    return (INode*)blk;
}

// Parse a return statement
INode *parseReturn(ParseState *parse) {
    ReturnNode *stmtnode = newReturnNode();
    lexNextToken(); // Skip past 'return'
    if (!lexIsEndOfStatement())
        stmtnode->exp = parseAnyExpr(parse);
    return parseFlowSuffix(parse, (INode*)stmtnode);
}

// Parse if statement/expression
INode *parseIf(ParseState *parse) {
    IfNode *ifnode = newIfNode();
    lexNextToken();
    nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
    nodesAdd(&ifnode->condblk, parseBlockOrStmt(parse));
    while (1) {
        // Process final else clause and break loop
        // Note: this code makes "else if" equivalent to "elif"
        if (lexIsToken(ElseToken)) {
            lexNextToken();
            if (!lexIsToken(IfToken)) {
                nodesAdd(&ifnode->condblk, voidType); // else distinguished by a 'void' condition
                nodesAdd(&ifnode->condblk, parseBlockOrStmt(parse));
                break;
            }
        }
        else if (!lexIsToken(ElifToken))
            break;

        // Elif processing
        lexNextToken();
        nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
        nodesAdd(&ifnode->condblk, parseBlockOrStmt(parse));
    }
    return (INode *)ifnode;
}

VarDclNode *parseBindVarDcl(ParseState *parse) {
    INode *perm = parsePerm(NULL);
    INode *permdcl = itypeGetTypeDcl(perm);
    if (permdcl != (INode*)mutPerm && permdcl != (INode*)immPerm)
        errorMsgNode(perm, ErrorInvType, "Permission not valid for pattern match binding");

    // Obtain variable's name
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected variable name for declaration");
        return newVarDclFull(anonName, VarDclTag, voidType, perm, NULL);
    }
    VarDclNode *varnode = newVarDclNode(lex->val.ident, VarDclTag, perm);
    lexNextToken();

    // Get type
    INode *vtype;
    if ((vtype = parseVtype(parse)))
        varnode->vtype = vtype == voidType ? NULL : vtype;
    else {
        errorMsgLex(ErrorInvType, "Expected type specification for pattern match binding");
        varnode->vtype = voidType;
    }

    return varnode;
}

// De-sugar a variable bound pattern match
void parseBoundMatch(ParseState *parse, IfNode *ifnode, NameUseNode *expnamenode) {
    // We will desugar a variable declaration into using a pattern match and re-cast
    CastNode *isnode = newIsNode((INode*)expnamenode, NULL);
    CastNode *castnode = newCastNode((INode*)expnamenode, NULL);

    // Parse the variable-bind into a vardcl,
    // then preserve its desired type into both the 'is' and 'cast' nodes
    VarDclNode *varnode = parseBindVarDcl(parse);
    isnode->typ = castnode->typ = castnode->vtype = varnode->vtype;
    varnode->value = (INode *)castnode;
    nodesAdd(&ifnode->condblk, (INode*)isnode);

    // Create and-then block, with vardcl injected at start
    if (lexIsToken(ColonToken))
        lexNextToken();
    BlockNode *blknode = (BlockNode*)parseBlockOrStmt(parse);
    nodesInsert(&blknode->stmts, (INode*)varnode, 0); // Inject vardcl at start of block
    nodesAdd(&ifnode->condblk, (INode*) blknode);
}

// Parse match expression, which is sugar translated to an 'if' block
INode *parseMatch(ParseState *parse) {
    // 'match' is de-sugared into a block:
    // - vardcl that capture the expression in a variable
    // - if .. elif .. else sequence for all the match cases
    BlockNode *blknode = newBlockNode();
    IfNode *ifnode = newIfNode();

    // Pick up the expression in a variable, then start the block
    lexNextToken();
    VarDclNode *expdclnode = newVarDclNode(anonName, VarDclTag, (INode*)immPerm);
    NameUseNode *expnamenode = newNameUseNode(anonName);
    expnamenode->tag = VarNameUseTag;
    expnamenode->dclnode = (INode*)expdclnode;
    expdclnode->value = parseSimpleExpr(parse);
    if (!lexIsToken(LCurlyToken)) {
        errorMsgLex(ErrorBadTok, "Expected opening block brace '{' after match expression");
        return (INode*)blknode;
    }
    lexNextToken();

    // Parse all cases
    while (!lexIsToken(RCurlyToken)) {
        switch (lex->toktype) {
        case PermToken:
            parseBoundMatch(parse, ifnode, expnamenode);
            break;
        case IsToken: {
            CastNode *isnode = newIsNode((INode*)expnamenode, NULL);
            lexNextToken();
            isnode->typ = parseVtype(parse);
            nodesAdd(&ifnode->condblk, (INode*)isnode);
            if (lexIsToken(ColonToken))
                lexNextToken();
            nodesAdd(&ifnode->condblk, parseBlockOrStmt(parse));
            break;
        }

        case EqToken: {
            FnCallNode *callnode = newFnCallOp((INode*)expnamenode, "==", 2);
            lexNextToken();
            nodesAdd(&callnode->args, parseSimpleExpr(parse));
            nodesAdd(&ifnode->condblk, (INode*)callnode);
            if (lexIsToken(ColonToken))
                lexNextToken();
            nodesAdd(&ifnode->condblk, parseBlockOrStmt(parse));
            break;
        }

        case ElseToken:
            lexNextToken();
            nodesAdd(&ifnode->condblk, voidType); // else distinguished by a 'void' condition
            nodesAdd(&ifnode->condblk, parseBlockOrStmt(parse));
            break;

        default:
            nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
            if (lexIsToken(ColonToken))
                lexNextToken();
            nodesAdd(&ifnode->condblk, parseBlockOrStmt(parse));
        }
    }
    parseRCurly();

    nodesAdd(&blknode->stmts, (INode*)expdclnode);
    nodesAdd(&blknode->stmts, (INode*)ifnode);
    return (INode *)blknode;
}

// Parse loop block
INode *parseLoop(ParseState *parse, LifetimeNode *life) {
    LoopNode *loopnode = newLoopNode();
    loopnode->life = life;
    lexNextToken();
    loopnode->blk = parseBlock(parse);
    return (INode *)loopnode;
}

// Parse while block
INode *parseWhile(ParseState *parse, LifetimeNode *life) {
    LoopNode *loopnode = newLoopNode();
    loopnode->life = life;
    lexNextToken();
    INode *condexp = parseSimpleExpr(parse);
    INode *blk = loopnode->blk = parseBlock(parse);
    parseInsertWhileBreak(blk, condexp);
    return (INode *)loopnode;
}

// Parse each block
INode *parseEach(ParseState *parse, INode *innerblk, LifetimeNode *life) {
    BlockNode *outerblk = newBlockNode();   // surrounding block scope for isolating 'each' vars
    LoopNode *loopnode = newLoopNode();
    loopnode->life = life;

    // Obtain all the parsed pieces
    lexNextToken();
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoVar, "Missing variable name");
        return (INode *)outerblk;
    }
    Name* elemname = lex->val.ident;
    lexNextToken();
    if (!lexIsToken(InToken)) {
        errorMsgLex(ErrorBadTok, "Missing 'in'");
        return (INode *)outerblk;
    }
    lexNextToken();
    INode *iter = parseSimpleExpr(parse);
    INode *step = NULL;
    int isrange = 0;
    if (iter->tag == FnCallTag && ((FnCallNode*)iter)->methfld) {
        Name *methodnm = ((NameUseNode*)((FnCallNode*)iter)->methfld)->namesym;
        if (methodnm == leName || methodnm == ltName)
            isrange = 1;
        else if (methodnm == geName || methodnm == gtName)
            isrange = -1;
    }
    if (isrange && lexIsToken(ByToken)) {
        lexNextToken();
        step = parseSimpleExpr(parse);
    }
    if (innerblk == NULL)
        innerblk = parseBlock(parse);
    loopnode->blk = innerblk;

    // Assemble logic for a range (with optional step), e.g.:
    // { mut elemname = initial; while elemname <= iterend { ... ; elemname += step}}
    if (isrange) {
        FnCallNode *itercmp = (FnCallNode *)iter;
        VarDclNode *elemdcl = newVarDclNode(elemname, VarDclTag, (INode*)mutPerm);
        elemdcl->value = itercmp->objfn;
        nodesAdd(&((BlockNode*)outerblk)->stmts, (INode*)elemdcl);
        itercmp->objfn = (INode*)newNameUseNode(elemname);
        if (step) {
            FnCallNode *pluseq = newFnCallOpname((INode*)newNameUseNode(elemname), plusEqName, 1);
            pluseq->flags |= FlagLvalOp;
            nodesAdd(&pluseq->args, step);
            nodesAdd(&((BlockNode*)loopnode->blk)->stmts, (INode*)pluseq);
        }
        else {
            INode *incr = (INode *)newFnCallOpname((INode *)newNameUseNode(elemname), isrange > 0 ? incrPostName : decrPostName, 0);
            nodesAdd(&((BlockNode*)loopnode->blk)->stmts, incr);
        }
        parseInsertWhileBreak(innerblk, iter);
        nodesAdd(&outerblk->stmts, (INode*)loopnode);
    }
    return (INode *)outerblk;
}

// Parse a lifetime variable, followed by colon and then a loop
// 'stmtflag' indicates it is a statement vs. an expression (loop)
INode *parseLifetime(ParseState *parse, int stmtflag) {
    LifetimeNode *life = newLifetimeDclNode(lex->val.ident, 0);
    lexNextToken();
    if (lexIsToken(ColonToken))
        lexNextToken();
    else
        errorMsgLex(ErrorBadTok, "Missing ':' after lifetime");

    if (lexIsToken(LoopToken))
        return parseLoop(parse, life);
    if (stmtflag) {
        if (lexIsToken(WhileToken))
            return parseWhile(parse, life);
        else if (lexIsToken(EachToken))
            return parseEach(parse, NULL, life);
    }
    errorMsgLex(ErrorBadTok, "A lifetime may only be followed by a loop/while/each");
    return NULL;
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

        case MatchToken:
            nodesAdd(&blk->stmts, parseMatch(parse));
            break;

        case LoopToken:
            nodesAdd(&blk->stmts, parseLoop(parse, NULL));
            break;

        case WhileToken:
            nodesAdd(&blk->stmts, parseWhile(parse, NULL));
            break;

        case EachToken:
            nodesAdd(&blk->stmts, parseEach(parse, NULL, NULL));
            break;

        case LifetimeToken:
            nodesAdd(&blk->stmts, parseLifetime(parse, 1));
            break;

        case BreakToken:
        {
            BreakNode *node = newBreakNode();
            lexNextToken();
            if (lexIsToken(LifetimeToken)) {
                node->life = (INode*)newNameUseNode(lex->val.ident);
                lexNextToken();
            }
            if (!(lexIsEndOfStatement() || lexIsToken(IfToken)))
                node->exp = parseAnyExpr(parse);
            nodesAdd(&blk->stmts, parseFlowSuffix(parse, (INode*)node));
            break;
        }

        case ContinueToken:
        {
            ContinueNode *node = newContinueNode();
            lexNextToken();
            if (lexIsToken(LifetimeToken)) {
                node->life = (INode*)newNameUseNode(lex->val.ident);
                lexNextToken();
            }
            nodesAdd(&blk->stmts, parseFlowSuffix(parse, (INode*)node));
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
            parseEndOfStatement();
            break;

        default:
            nodesAdd(&blk->stmts, parseExpStmt(parse));
        }
    }

    parseRCurly();
    return (INode*)blk;
}
