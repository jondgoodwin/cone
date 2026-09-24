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

// Build 'if condexp {break}', or 'if !condexp {break}' when 'unless' is set.
// The break names the loop's lifetime when it has one, so that a copy of it
// carried into an inner loop -- ahead of a labelled 'continue' -- still leaves
// the loop it was built for rather than the innermost one.
static IfNode *parseBreakIf(INode *condexp, int unless, Name *lifesym) {
    BreakRetNode *breaknode = newBreakNode();
    inodeLexCopy((INode*)breaknode, condexp);
    breaknode->exp = (INode*)newNilLitNode();
    if (lifesym)
        breaknode->life = (INode*)newNameUseFromLex(lifesym, condexp);
    BlockNode *ifblk = newBlockNode();
    inodeLexCopy((INode*)ifblk, condexp);
    nodesAdd(&ifblk->stmts, (INode*)breaknode);
    INode *cond = condexp;
    if (unless) {
        LogicNode *notiter = newLogicNode(NotLogicTag);
        inodeLexCopy((INode*)notiter, condexp);
        notiter->lexp = condexp;
        cond = (INode*)notiter;
    }
    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, condexp);
    nodesAdd(&ifnode->condblk, cond);
    nodesAdd(&ifnode->condblk, (INode *)ifblk);
    return ifnode;
}

// This helper routine inserts 'break if !condexp' at beginning of block
void parseInsertWhileBreak(INode *blk, INode *condexp) {
    nodesInsert(&((BlockNode*)blk)->stmts, (INode*)parseBreakIf(condexp, 1, NULL), 0);
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
    if ((vtype = parseType(parse)) != unknownType)
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

    // Parse the variable-bind into a vardcl, then move its desired type into both
    // the 'is' and 'cast' nodes. The pattern's bare root name is looked up in the
    // matched value's enum at type check (castPatternBind), so the variable takes
    // its type from the conversion, once that is bound, rather than holding a
    // third copy of a node a clone would leave unbound.
    VarDclNode *varnode = parseBindVarDcl(parse);
    castPatternMark(varnode->vtype);
    isnode->typ = castnode->typ = castnode->vtype = varnode->vtype;
    castnode->flags |= FlagMatchBind;
    varnode->vtype = unknownType;
    varnode->value = (INode *)castnode;

    // If value expression is needed, obtain it also
    if (valnode != NULL) {
        if (lexIsToken(AssgnToken))
            lexNextToken();
        else {
            errorMsgLex(ErrorInvType, "Expected '=' followed by value to match against");
        }
        valnode->value = parseSimpleExpr(parse);
        nodesAdd(&ifnode->condblk, (INode*)isnode);
    }
    // A match's case may end in an 'if' guard, which may name the variable. The
    // variable is declared in the arm, which the guard is not in, so the guard is
    // a block that binds it again, to the same conversion of the same value:
    //   'case imm x T if g {...}'  ->  'is T and {imm x = [T]v; g}' then '{imm x = [T]v; ...}'
    // Being an 'and', the condition is not an 'is', so exhaustiveness does not
    // count the arm, which is right: the guard may fail.
    else if (lexIsToken(IfToken)) {
        lexNextToken();
        CastNode *guardcast = newConvCastNode((INode*)expnamenode, castnode->typ);
        guardcast->vtype = castnode->typ;
        guardcast->flags |= FlagMatchBind;
        VarDclNode *guardvar = newVarDclFull(varnode->namesym, VarDclTag, unknownType, varnode->perm, (INode*)guardcast);
        BlockNode *guardblk = newBlockNode();
        nodesAdd(&guardblk->stmts, (INode*)guardvar);
        nodesAdd(&guardblk->stmts, parseSimpleExpr(parse));
        LogicNode *guarded = newLogicNode(AndLogicTag);
        guarded->lexp = (INode*)isnode;
        guarded->rexp = (INode*)guardblk;
        nodesAdd(&ifnode->condblk, (INode*)guarded);
    }
    else
        nodesAdd(&ifnode->condblk, (INode*)isnode);

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

// Finish a range pattern, whose lower bound is parsed and whose '..' or '...'
// is the current token. It tests the matched value against both bounds:
// 'a .. b' is 'v >= a and v < b', and 'a ... b' is 'v >= a and v <= b'.
static INode *parseMatchRange(ParseState *parse, INode *matchee, INode *lower) {
    // All three nodes take the operator's position, so a matched value that
    // cannot be ordered is reported at the '..'
    FnCallNode *gecall = newFnCallOp(matchee, ">=", 2);
    FnCallNode *upcall = newFnCallOp(matchee, lexIsToken(EllipsisToken) ? "<=" : "<", 2);
    LogicNode *range = newLogicNode(AndLogicTag);
    lexNextToken();
    nodesAdd(&gecall->args, lower);
    nodesAdd(&upcall->args, parseOr(parse));
    range->lexp = (INode *)gecall;
    range->rexp = (INode *)upcall;
    return (INode *)range;
}

// Parse one pattern of a case, returning the condition that tests the matched
// value against it:
// - 'is T' narrows to a type, and its bare root name is looked up in the matched
//   value's enum (castPatternMark)
// - a comparison operator and a value, '==v', '<v', '!=v' and the rest, compares
//   the matched value, the operator's left operand, with the value
// - 'a .. b' or 'a ... b' is a range (parseMatchRange)
// A value on its own is not a pattern: whether it means '==' is not decided,
// and today a case that begins with one is a condition, not a comparison.
static INode *parseMatchPattern(ParseState *parse, INode *matchee) {
    if (lexIsToken(IsToken)) {
        CastNode *isnode = newIsNode(matchee, unknownType);
        lexNextToken();
        isnode->typ = parseType(parse);
        castPatternMark(isnode->typ);
        return (INode *)isnode;
    }
    char *cmpop = parseCmpOp();
    if (cmpop != NULL) {
        FnCallNode *callnode = newFnCallOp(matchee, cmpop, 2);
        lexNextToken();
        nodesAdd(&callnode->args, parseOr(parse));
        return (INode *)callnode;
    }
    INode *value = parseOr(parse);
    if (lexIsToken(DotDotToken) || lexIsToken(EllipsisToken))
        return parseMatchRange(parse, matchee, value);
    errorMsgNode(value, ErrorPatBare,
        "A value alone is not a pattern. Write '==' before it to compare the matched value with it.");
    return value;
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
    expnamenode->dclnode = (INode*)expdclnode;
    expdclnode->value = parseSimpleExpr(parse);

    // Parse all cases
    parseBlockStart();
    while (!parseBlockEnd()) {
        // Handle pattern that begins with 'case'
        if (lexIsToken(CaseToken)) {
            lexNextToken(); // consume the 'case' token
            // Handle bound variable pattern
            if (lexIsToken(PermToken)) {
                parseBoundMatch(parse, ifnode, expnamenode, NULL);
                continue;
            }

            // Anything else is one or more patterns joined by 'or', or a
            // condition, then an optional 'if' guard. A case that begins with
            // neither 'is' nor a comparison operator is a range pattern when
            // '..' or '...' follows its first operand, and otherwise a
            // condition, whose own 'or' the condition has already taken.
            INode *cond;
            int patterns = 1;
            if (lexIsToken(IsToken) || parseCmpOp() != NULL)
                cond = parseMatchPattern(parse, (INode *)expnamenode);
            else if (lexIsToken(NotToken)) {
                cond = parseSimpleExpr(parse);
                patterns = 0;
            }
            else {
                INode *first = parseOr(parse);
                if (lexIsToken(DotDotToken) || lexIsToken(EllipsisToken))
                    cond = parseMatchRange(parse, (INode *)expnamenode, first);
                else {
                    cond = parseSimpleExprFrom(parse, first);
                    patterns = 0;
                }
            }
            while (patterns && lexIsToken(OrToken)) {
                LogicNode *ornode = newLogicNode(OrLogicTag);
                lexNextToken();
                ornode->lexp = cond;
                ornode->rexp = parseMatchPattern(parse, (INode *)expnamenode);
                cond = (INode *)ornode;
            }
            if (lexIsToken(IfToken)) {
                LogicNode *guarded = newLogicNode(AndLogicTag);
                lexNextToken();
                guarded->lexp = cond;
                guarded->rexp = parseSimpleExpr(parse);
                cond = (INode *)guarded;
            }
            nodesAdd(&ifnode->condblk, cond);
            nodesAdd(&ifnode->condblk, parseExprBlock(parse, 0));
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
        // Every node below is built after parseExprBlock has consumed the whole
        // loop body, so the lexer sits on the token following the body's '}' --
        // which is usually the enclosing function's. Position them on the range
        // expression instead, since that is what the reader wrote and what the
        // diagnostic is really about.
        VarDclNode *elemdcl = newVarDclNode(elemname, VarDclTag, (INode*)mutPerm);
        inodeLexCopy((INode*)elemdcl, iter);
        elemdcl->value = itercmp->objfn;
        nodesAdd(&((BlockNode*)outerblk)->stmts, (INode*)elemdcl);
        itercmp->objfn = (INode*)newNameUseFromLex(elemname, iter);
        if (step) {
            FnCallNode *pluseq = newFnCallOpnameLower(iter, (INode*)newNameUseFromLex(elemname, iter), plusEqName, 1);
            pluseq->flags |= FlagOpAssgn | FlagLvalOp;
            nodesAdd(&pluseq->args, step);
            // A step of more than one need never land on the bound, so nothing
            // stops it carrying the loop variable past the type's extreme: it
            // wraps to the other end, satisfies the comparison again, and the
            // loop never ends. The bound cannot be consulted ahead of the step
            // to see that coming -- the distance to it overflows on a signed
            // range wider than half its type, and whether the step adds or
            // subtracts is not known until it has been evaluated -- but the wrap
            // is plain afterwards: the loop variable moved against the range's
            // direction. So the step is '{ imm prev = x; x += s; if x < prev
            // {break} }', with '>' for a range counting down. The three stay one
            // statement, because the trailing statement is what a 'continue'
            // carries a copy of, and 'prev' is a phantom variable the copy
            // re-points at its own declaration.
            VarDclNode *prevdcl = newVarDclFull(anonName, VarDclTag, unknownType, (INode*)immPerm,
                (INode*)newNameUseFromLex(elemname, iter));
            inodeLexCopy((INode*)prevdcl, iter);
            NameUseNode *prevuse = newNameUseFromLex(anonName, iter);
            prevuse->dclnode = (INode*)prevdcl;
            FnCallNode *wrapped = newFnCallOpnameLower(iter, (INode*)newNameUseFromLex(elemname, iter),
                isrange > 0 ? ltName : gtName, 1);
            nodesAdd(&wrapped->args, (INode*)prevuse);
            BlockNode *stepblk = newBlockNode();
            inodeLexCopy((INode*)stepblk, iter);
            nodesAdd(&stepblk->stmts, (INode*)prevdcl);
            nodesAdd(&stepblk->stmts, (INode*)pluseq);
            nodesAdd(&stepblk->stmts, (INode*)parseBreakIf((INode*)wrapped, 0, lifesym));
            nodesAdd(&loopnode->stmts, (INode*)stepblk);
        }
        else {
            INode *incr = (INode *)newFnCallOpnameLower(iter, (INode *)newNameUseFromLex(elemname, iter),
                isrange > 0 ? incrPostName : decrPostName, 0);
            incr->flags |= FlagLvalOp;
            Name *cmpname = ((NameUseNode*)itercmp->methfld)->namesym;
            if (cmpname == leName || cmpname == geName) {
                // An inclusive range's last value is its bound, and the bound may
                // be the type's maximum (or minimum, counting down). Stepping past
                // it wraps, the wrapped value passes the guard again, and the loop
                // never ends -- LLVM folds 'x <= MAX' to true and emits a loop with
                // no exit. So the step is guarded: '{ if x == bound {break}; x++ }'.
                // The two stay one statement, because the trailing statement is
                // what a 'continue' carries a copy of. The bound is cloned rather
                // than shared with the guard: a node reachable twice in the tree
                // is type checked twice.
                CloneState cstate;
                cstate.instnode = NULL;
                cstate.selftype = NULL;
                cstate.selfparm = NULL;
                cstate.scope = 0;
                FnCallNode *atbound = newFnCallOpnameLower(iter, (INode*)newNameUseFromLex(elemname, iter), eqName, 1);
                nodesAdd(&atbound->args, cloneNode(&cstate, nodesGet(itercmp->args, 0)));
                BlockNode *stepblk = newBlockNode();
                inodeLexCopy((INode*)stepblk, iter);
                nodesAdd(&stepblk->stmts, (INode*)parseBreakIf((INode*)atbound, 0, lifesym));
                nodesAdd(&stepblk->stmts, incr);
                incr = (INode*)stepblk;
            }
            nodesAdd(&loopnode->stmts, incr);
        }
        // The step is now the block's last statement and stays there: the guard
        // below is inserted at index 0, blockTypeCheck appends no 'blockret' to a
        // loop block, and blockFlow runs later. Saying so is what lets name
        // resolution find the step to copy ahead of a 'continue'.
        loopnode->flags |= FlagLoopStep;
        parseInsertWhileBreak((INode*)loopnode, iter);
        nodesAdd(&outerblk->stmts, (INode*)loopnode);
    }
    else {
        // Only the numeric range operators are rewritten into a loop. Anything
        // else -- a collection, a slice, a closure iterator -- has no iteration
        // protocol behind it yet, so the loop and its body are dropped. Saying
        // so is the difference between an unimplemented feature and a statement
        // that silently does nothing.
        errorMsgNode(iter, ErrorNotIterable,
            "'each' can only iterate over a numeric range, such as 'each x in 0 < n'.");
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
        switch (lex->toktype) {
        case SemiToken:
            lexNextToken();
            break;

        // A local is seen by its block and by nothing else, so there is nothing
        // for 'pub' to make visible
        case PubToken:
            errorMsgLex(ErrorBadPub, "'pub' may not precede a local declaration; only a module's or a type's members have an outside to be visible from");
            lexNextToken();
            break;

        // One copy shared by every call of the function, rather than one per
        // call. Storage is a global (genlLocalVar), so the initializer is a
        // literal as a global's is, and the block does not release it.
        case StaticToken: {
            uint16_t staticflag = parseStatic();
            if (!lexIsToken(PermToken) && !lexIsToken(IdentToken)) {
                parseBadStatic(staticflag);
                break;
            }
            VarDclNode *var = parseVarDcl(parse, immPerm, ParseMaySig | ParseMayImpl);
            var->flags |= staticflag;
            var->flowtempflags |= VarInitialized;   // A static holds a valid value from the start, as a global does
            parseEndOfStatement();
            nodesAdd(&blk->stmts, (INode*)var);
            break;
        }

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
            nodesAdd(&blk->stmts, (INode*)parseVarDcl(parse, immPerm, ParseMaySig|ParseMayImpl));
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
        // A second name straight after the first is what a constraint or a
        // parameter type is spelled as -- 'fn max[T Comparable]', which the
        // reference manual shows. Neither is implemented, and reading the two
        // names as two parameters instead turned that into an arity or
        // inference complaint about a declaration written in the documented
        // form. Refuse it here and resync to the next ',' or ']'.
        else if (lexIsToken(IdentToken)) {
            errorMsgLex(ErrorGenParmConstr, "A type parameter may not carry a constraint or a type: neither is implemented. Separate two parameters with a comma.");
            while (!lexIsToken(CommaToken) && !lexIsToken(RBracketToken)) {
                if (lexIsToken(SemiToken) || lexIsToken(LCurlyToken) || lexIsToken(RCurlyToken) || lexIsToken(EofToken))
                    break;
                lexNextToken();
            }
            if (lexIsToken(CommaToken))
                lexNextToken();
        }
    }
    if (lexIsToken(RBracketToken))
        lexNextToken();
    else
        errorMsgLex(ErrorBadTok, "Expected list of macro/generic parameter ending with square bracket.");
    // 'fn f[]()' declares a generic with nothing to substitute, so no call can
    // ever instantiate it and the declaration would generate nothing at all.
    // That is worth saying rather than leaving the function silently absent.
    if (parms->used == 0)
        errorMsgLex(ErrorNoGenParms, "A type parameter list must declare at least one parameter.");
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

    // '@c' after the keyword: this function's symbol is a C name, its own name
    // or the string written, and 'system' its calling convention [Jon 23 Sep].
    // '@initpure' there too, before or after it: a function a module's 'init'
    // may call, as 'init' itself is declared (refmodule.html, "Dynamic
    // initialization"). Recorded, not yet checked
    int initpure = 0;
    if (lexIsToken(InitPureToken)) {
        initpure = 1;
        lexNextToken();
    }
    int hasc = parseCAttr(&fnnode->dclinfo, 0);
    if (!initpure && lexIsToken(InitPureToken)) {
        initpure = 1;
        lexNextToken();
    }
    if (initpure)
        fnnode->dclinfo.facts |= DclInitPure;

    // Process function name, if provided
    if (lexIsToken(IdentToken)) {
        if (!(mayflags&ParseMayName))
            errorMsgLex(WarnName, "Unnecessary function name is ignored");
        fnnode->namesym = lex->val.ident;
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

    // Process the optional overload name this declaration also answers to.
    // The concrete name stays this declaration's own identity; the overload name
    // is shared with the other declarations that overload it.
    if (lexIsToken(OverloadToken)) {
        lexNextToken();
        if (!lexIsToken(IdentToken)) {
            errorMsgLex(ErrorBadOverload, "Expected the overload name that follows 'overload'");
        }
        else {
            Name *overloadsym = lex->val.ident;
            lexNextToken();
            if (fnnode->namesym == NULL)
                errorMsgLex(ErrorBadOverload, "An anonymous function may not declare an overload name");
            else if (fnnode->namesym == overloadsym)
                errorMsgLex(ErrorBadOverload,
                    "A declaration's overload name must differ from its own name %s", &overloadsym->namestr);
            else if (fnnode->genericinfo)
                errorMsgLex(ErrorGenericOverload,
                    "A generic function may not declare the overload name %s", &overloadsym->namestr);
            else
                fnnode->overloadsym = overloadsym;
        }
    }

    // Process the function's signature info.
    fnnode->vtype = parseFnSig(parse);

    // Handle optional specification that we are declaring an inline function,
    // one whose implementation will be "inlined" into any function that calls it
    if (lexIsToken(InlineToken)) {
        fnnode->flags |= FlagInline;
        lexNextToken();
    }

    // '@c' names a symbol, so it goes only where a function has one of its own
    if (hasc) {
        INsTypeNode *type = parse->typenode;
        const char *why = NULL;
        if (fnnode->namesym == NULL)
            why = "An anonymous function has no name for '@c' to give C spelling to.";
        else if (fnnode->genericinfo)
            why = "A generic function has a symbol per instance, each spelled with its type arguments, so '@c' cannot name them.";
        else if (fnnode->flags & FlagInline)
            why = "An inline function is expanded where it is called and leaves no symbol for '@c' to name.";
        else if (type && type->tag == StructTag
            && ((type->flags & TraitType) || ((StructNode*)type)->genericinfo))
            why = "A trait's method is copied into each type that implements it, and a generic type's into each instance, so no one symbol is there for '@c' to name.";
        if (why) {
            errorMsgNode((INode*)fnnode, ErrorCAttr, "%s", why);
            fnnode->dclinfo.facts &= ~DclStated;
            fnnode->dclinfo.cname = NULL;
        }
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
