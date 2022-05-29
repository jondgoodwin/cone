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

INode *parseEach(ParseState *parse, Name *lifesym, int stmtflag);

// This helper routine inserts 'break if !condexp' at beginning of block
void parseInsertWhileBreak(INode *blk, INode *condexp) {
    BreakRetNode *breaknode = newBreakNode();
    inodeLexCopy((INode*)breaknode, condexp);
    breaknode->exp = (INode*)newNilLitNode();
    BlockNode *ifblk = newBlockNode();
    inodeLexCopy((INode*)ifblk, condexp);
    nodesAdd(&ifblk->stmts, (INode*)breaknode);
    LogicNode *notiter = newLogicNode(NotLogicTag);
    inodeLexCopy((INode*)notiter, condexp);
    notiter->lexp = condexp;
    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, condexp);
    nodesAdd(&ifnode->condblk, (INode *)notiter);
    nodesAdd(&ifnode->condblk, (INode *)ifblk);
    nodesInsert(&((BlockNode*)blk)->stmts, (INode*)ifnode, 0);
}

// Parse an expression statement within a function
INode *parseExpStmt(ParseState *parse) {
    INode *node = parseAnyExpr(parse);
    parseEndOfStatement();
    return node;
}

// Parse a return statement
INode *parseReturn(ParseState *parse) {
    BreakRetNode *stmtnode = newReturnNode();
    lexNextToken(); // Skip past 'return'
    stmtnode->exp = parseIsEndOfStatement()? (INode*)newNilLitNode() : parseAnyExpr(parse);
    parseEndOfStatement();
    return (INode*)stmtnode;
}

// Parses a variable bound to a pattern match on a value
// (it looks like, and is returned as, a variable declaration)
VarDclNode *parseBindVarDcl(ParseState *parse) {
    INode *perm = parsePerm();
    INode *permdcl = perm==unknownType? unknownType : itypeGetTypeDcl(perm);
    if (permdcl != (INode*)mutPerm && permdcl != (INode*)immPerm)
        errorMsgNode(perm, ErrorInvType, "Permission not valid for pattern match binding");

    // Obtain variable's name
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoIdent, "Expected variable name for declaration");
        return newVarDclFull(anonName, VarDclTag, unknownType, perm, NULL);
    }
    VarDclNode *varnode = newVarDclNode(lex->val.ident, VarDclTag, perm);
    lexNextToken();

    // Get type
    INode *vtype;
    if ((vtype = parseVtype(parse)) != unknownType)
        varnode->vtype = vtype;
    else {
        errorMsgLex(ErrorInvType, "Expected type specification for pattern match binding");
        varnode->vtype = unknownType;
    }

    return varnode;
}

// De-sugar a variable bound pattern match
void parseBoundMatch(ParseState *parse, IfNode *ifnode, NameUseNode *expnamenode, VarDclNode *valnode) {
    // We will desugar a variable declaration into using a pattern match and re-cast
    CastNode *isnode = newIsNode((INode*)expnamenode, unknownType);
    CastNode *castnode = newConvCastNode((INode*)expnamenode, unknownType);

    // Parse the variable-bind into a vardcl,
    // then preserve its desired type into both the 'is' and 'cast' nodes
    VarDclNode *varnode = parseBindVarDcl(parse);
    isnode->typ = castnode->typ = castnode->vtype = varnode->vtype;
    varnode->value = (INode *)castnode;
    nodesAdd(&ifnode->condblk, (INode*)isnode);

    // If value expression is needed, obtain it also
    if (valnode != NULL) {
        if (lexIsToken(AssgnToken))
            lexNextToken();
        else {
            errorMsgLex(ErrorInvType, "Expected '=' followed by value to match against");
        }
        valnode->value = parseSimpleExpr(parse);
    }

    // Create and-then block, with vardcl injected at start
    BlockNode *blknode = (BlockNode*)parseExprBlock(parse, 0);
    nodesInsert(&blknode->stmts, (INode*)varnode, 0); // Inject vardcl at start of block
    nodesAdd(&ifnode->condblk, (INode*)blknode);
}

// Parse if statement/expression
INode *parseIf(ParseState *parse) {
    IfNode *ifnode = newIfNode();
    INode *retnode = (INode*)ifnode;
    lexNextToken();
    // To handle bound pattern match, we need to de-sugar:
    // - 'if' is wrapped in a block, where we first capture the value in a variable
    // - The conditional turns into an 'is' check
    // - The first statement in the block actually binds the var to the re-cast value
    if (lexIsToken(PermToken)) {
        BlockNode *blknode = newBlockNode();
        VarDclNode *valnode = newVarDclFull(anonName, VarDclTag, unknownType, (INode*)immPerm, NULL);
        NameUseNode *valnamenode = newNameUseNode(anonName);
        valnamenode->tag = VarNameUseTag;
        valnamenode->dclnode = (INode*)valnode;
        nodesAdd(&blknode->stmts, (INode*)valnode);
        nodesAdd(&blknode->stmts, (INode*)ifnode);
        retnode = (INode*)blknode; // return block instead of 'if'!
        parseBoundMatch(parse, ifnode, valnamenode, valnode);
    }
    else {
        nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
        nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
    }
    while (1) {
        // Process final else clause and break loop
        // Note: this code makes "else if" equivalent to "elif"
        if (lexIsToken(ElseToken)) {
            lexNextToken();
            if (!lexIsToken(IfToken)) {
                nodesAdd(&ifnode->condblk, elseCond); // else distinguished by a elseCond
                nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
                break;
            }
        }
        else if (!lexIsToken(ElifToken))
            break;

        // Elif processing
        lexNextToken();
        // To handle bound pattern match, we need to de-sugar:
        // - 'if' is wrapped in a block, where we first capture the value in a variable
        // - The conditional turns into an 'is' check
        // - The first statement in the block actually binds the var to the re-cast value
        if (lexIsToken(PermToken)) {
            BlockNode *blknode = newBlockNode();
            nodesAdd(&ifnode->condblk, elseCond);
            nodesAdd(&ifnode->condblk, (INode*)blknode);
            VarDclNode *valnode = newVarDclFull(anonName, VarDclTag, unknownType, (INode*)immPerm, NULL);
            NameUseNode *valnamenode = newNameUseNode(anonName);
            valnamenode->tag = VarNameUseTag;
            valnamenode->dclnode = (INode*)valnode;
            nodesAdd(&blknode->stmts, (INode*)valnode);
            ifnode = newIfNode();
            nodesAdd(&blknode->stmts, (INode*)ifnode);
            parseBoundMatch(parse, ifnode, valnamenode, valnode);
        }
        else {
            nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
            nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
        }
    }
    return retnode;
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

    // Parse all cases
    parseBlockStart();
    while (!parseBlockEnd()) {
        lexStmtStart();
        // Handle pattern that begins with 'case'
        if (lexIsToken(CaseToken)) {
            lexNextToken(); // consume the 'case' token
            // Handle bound variable pattern
            if (lexIsToken(PermToken)) {
                parseBoundMatch(parse, ifnode, expnamenode, NULL);
            }
            else if (lexIsToken(IsToken)) {
                CastNode *isnode = newIsNode((INode *)expnamenode, unknownType);
                lexNextToken();
                isnode->typ = parseVtype(parse);
                nodesAdd(&ifnode->condblk, (INode *)isnode);
                nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
            } else if (lexIsToken(EqToken)) {
                FnCallNode *callnode = newFnCallOp((INode *)expnamenode, "==", 2);
                lexNextToken();
                nodesAdd(&callnode->args, parseSimpleExpr(parse));
                nodesAdd(&ifnode->condblk, (INode *)callnode);
                nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
            } else {
                nodesAdd(&ifnode->condblk, parseSimpleExpr(parse));
                nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
            }
        } else if (lexIsToken(ElseToken)) {
            lexNextToken();
            nodesAdd(&ifnode->condblk, elseCond); // else distinguished by a elseCond condition
            nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
        } else {
            errorMsgLex(ErrorBadTerm, "Parser Error: should be either case or else");
            return (INode *)blknode;
        }
    }
    nodesAdd(&blknode->stmts, (INode*)expdclnode);
    nodesAdd(&blknode->stmts, (INode*)ifnode);
    return (INode *)blknode;
}

// Parse while block
INode *parseWhile(ParseState *parse, Name *lifesym, int stmtflag) {
    lexNextToken();
    INode *condexp = NULL;
    if (!parseHasBlock()) {
        if (!stmtflag)
            errorMsg(ErrorNoLoop, "while with condition expression may not be used as an expression");
        condexp = parseSimpleExpr(parse);
    }
    BlockNode *loopnode = (BlockNode*)parseExprBlock(parse, 1);
    loopnode->lifesym = lifesym;
    if (condexp)
        parseInsertWhileBreak((INode*)loopnode, condexp);
    return (INode *)loopnode;
}

// Parse each block
INode *parseEach(ParseState *parse, Name *lifesym, int stmtflag) {
    if (!stmtflag)
        errorMsg(ErrorNoLoop, "each may not be used as an expression");
    BlockNode *outerblk = newBlockNode();   // surrounding block scope for isolating 'each' vars

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
    BlockNode *loopnode = (BlockNode*)parseExprBlock(parse, 1);
    loopnode->lifesym = lifesym;

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
            pluseq->flags |= FlagOpAssgn | FlagLvalOp;
            nodesAdd(&pluseq->args, step);
            nodesAdd(&loopnode->stmts, (INode*)pluseq);
        }
        else {
            INode *incr = (INode *)newFnCallOpname((INode *)newNameUseNode(elemname), isrange > 0 ? incrPostName : decrPostName, 0);
            incr->flags |= FlagLvalOp;
            nodesAdd(&loopnode->stmts, incr);
        }
        parseInsertWhileBreak((INode*)loopnode, iter);
        nodesAdd(&outerblk->stmts, (INode*)loopnode);
    }
    return (INode *)outerblk;
}

// Parse a lifetime variable, followed by colon and then a loop
// 'stmtflag' indicates it is a statement vs. an expression (loop)
INode *parseLifetime(ParseState *parse, int stmtflag) {
    Name *lifesym = lex->val.ident;
    lexNextToken();
    if (lexIsToken(ColonToken))
        lexNextToken();
    else
        errorMsgLex(ErrorBadTok, "Missing ':' after lifetime");

    if (lexIsToken(WhileToken))
        return parseWhile(parse, lifesym, stmtflag);
    else if (lexIsToken(EachToken))
        return parseEach(parse, lifesym, stmtflag);
    errorMsgLex(ErrorBadTok, "A lifetime may only be followed by a loop/while/each");
    return NULL;
}

// Parse a 'with' block, setting 'this' to the expression at start of block
INode *parseWith(ParseState *parse) {
    lexNextToken();
    VarDclNode *this = newVarDclFull(thisName, VarDclTag, unknownType, (INode*)immPerm, NULL);
    this->value = parseSimpleExpr(parse);
    BlockNode *blk = (BlockNode*)parseExprBlock(parse, 0);
    nodesInsert(&blk->stmts, (INode*)this, 0);
    return (INode *)blk;
}

// Parse a block of statements/expressions
INode *parseExprBlock(ParseState *parse, int isloop) {
    BlockNode *blk = isloop? newLoopBlockNode() : newBlockNode();
    if (blk->stmts == NULL)
        blk->stmts = newNodes(8);

    parseBlockStart();

    while (!parseBlockEnd()) {
        lexStmtStart();
        switch (lex->toktype) {
        case SemiToken:
            lexNextToken();
            break;

        case RetToken:
            nodesAdd(&blk->stmts, parseReturn(parse));
            break;

        case WithToken:
            nodesAdd(&blk->stmts, parseWith(parse));
            break;

        case IfToken:
            nodesAdd(&blk->stmts, parseIf(parse));
            break;

        case MatchToken:
            nodesAdd(&blk->stmts, parseMatch(parse));
            break;

        case WhileToken:
            nodesAdd(&blk->stmts, parseWhile(parse, NULL, 1));
            break;

        case EachToken:
            nodesAdd(&blk->stmts, parseEach(parse, NULL, 1));
            break;

        case LifetimeToken:
            nodesAdd(&blk->stmts, parseLifetime(parse, 1));
            break;

        case BreakToken:
        {
            BreakRetNode *node = newBreakNode();
            lexNextToken();
            if (lexIsToken(LifetimeToken)) {
                node->life = (INode*)newNameUseNode(lex->val.ident);
                lexNextToken();
            }
            node->exp = parseIsEndOfStatement()? (INode*)newNilLitNode() : parseAnyExpr(parse);
            parseEndOfStatement();
            nodesAdd(&blk->stmts, (INode*)node);
            break;
        }

        case ContinueToken:
        {
            BreakRetNode *node = newContinueNode();
            lexNextToken();
            if (lexIsToken(LifetimeToken)) {
                node->life = (INode*)newNameUseNode(lex->val.ident);
                lexNextToken();
            }
            parseEndOfStatement();
            nodesAdd(&blk->stmts, (INode*)node);
            break;
        }

        case LCurlyToken:
            nodesAdd(&blk->stmts, parseExprBlock(parse, 0));
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

    return (INode*)blk;
}

// Parse a list of generic variables and add to the genericnode
Nodes *parseGenericParms(ParseState *parse) {
    lexNextToken(); // Go past left square bracket
    Nodes *parms = newNodes(2);
    while (lexIsToken(IdentToken)) {
        GenVarDclNode *parm = newGVarDclNode(lex->val.ident);
        nodesAdd(&parms, (INode*)parm);
        lexNextToken();
        if (lexIsToken(CommaToken))
            lexNextToken();
    }
    if (lexIsToken(RBracketToken))
        lexNextToken();
    else
        errorMsgLex(ErrorBadTok, "Expected list of macro/generic parameter ending with square bracket.");
    return parms;
}

// Parse a macro declaration
MacroDclNode *parseMacro(ParseState *parse) {
    lexNextToken();
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorBadTok, "Expected a macro name");
        return newMacroDclNode(anonName);
    }
    MacroDclNode *macro = newMacroDclNode(lex->val.ident);
    lexNextToken();
    if (lexIsToken(LBracketToken)) {
        macro->parms = parseGenericParms(parse);
    }
    macro->body = parseExprBlock(parse, 0);
    return macro;
}

// Parse a function block
INode *parseFn(ParseState *parse, uint16_t mayflags) {
    FnDclNode *fnnode = newFnDclNode(NULL, 0, NULL, NULL);

    // Skip past the 'fn'.
    lexNextToken();

    // Process function name, if provided
    if (lexIsToken(IdentToken)) {
        if (!(mayflags&ParseMayName))
            errorMsgLex(WarnName, "Unnecessary function name is ignored");
        fnnode->namesym = lex->val.ident;
        fnnode->genname = &fnnode->namesym->namestr;
        lexNextToken();
        if (lexIsToken(LBracketToken)) {
            fnnode->genericinfo = newGenericInfo();
            fnnode->genericinfo->parms = parseGenericParms(parse);
        }
    }
    else {
        if (!(mayflags&ParseMayAnon))
            errorMsgLex(ErrorNoName, "Function declarations must be named");
    }

    // Process the function's signature info.
    fnnode->vtype = parseFnSig(parse);

    // Handle optional specification that we are declaring an inline function,
    // one whose implementation will be "inlined" into any function that calls it
    if (lexIsToken(InlineToken)) {
        fnnode->flags |= FlagInline;
        lexNextToken();
    }

    // Process statements block that implements function, if provided
    if (parseHasBlock()) {
        if (!(mayflags&ParseMayImpl))
            errorMsgNode((INode*)fnnode, ErrorBadImpl, "Function/method implementation is not allowed here.");
        fnnode->value = parseExprBlock(parse, 0);
    }
    else {
        if (!(mayflags&ParseMaySig))
            errorMsgNode((INode*)fnnode, ErrorNoImpl, "Function/method must be implemented.");
        if (!(mayflags&ParseEmbedded))
            parseEndOfStatement();
    }

    return (INode*)fnnode;
}
