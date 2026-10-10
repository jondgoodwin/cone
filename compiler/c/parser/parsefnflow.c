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
#include <string.h>

INode *parseEach(ParseState *parse, Name *lifesym, int stmtflag);

// The most variables an 'each' unpacks a tuple item into
#define EachMaxVars 8

// Which each is being parsed: one with a body, or an entry of '<-' (parseEachLoop)
enum { EachBuildNone, EachBuildVars, EachBuildDrain };

// Where the next 'if' read may leave its condition, instead of reading a block
// after it, when the statement ends there: the trailing 'if' of a 'break' or
// 'return' with no value (parseJumpValue). Cleared as parseIf starts.
static INode **parseTrailingIfOut = NULL;

// A use of the counter a range loop steps, which no name reaches: its uses are
// bound to its declaration, as a match's hidden variable's are, so a copy of the
// step carried into an inner loop by a labelled 'continue' still reaches this
// loop's counter
static INode *parseEachCounterUse(VarDclNode *counter, INode *lexnode) {
    NameUseNode *use = newNameUseFromLex(anonName, lexnode);
    use->dclnode = (INode*)counter;
    return (INode*)use;
}

// 'done = true', setting the flag of a range loop that is to leave through its
// 'else' at the start of the next pass
static INode *parseEachSetDone(VarDclNode *done, INode *lexnode) {
    // (the target is the flag's own name: an assignment to the anonymous name
    // discards its value)
    NameUseNode *target = newNameUseFromLex(done->namesym, lexnode);
    target->dclnode = (INode*)done;
    AssignNode *set = newAssignNode(NormalAssign, (INode*)target, (INode*)newULitNode(1, (INode*)boolType));
    inodeLexCopy((INode*)set, lexnode);
    return (INode*)set;
}

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

// Build 'if cond {jump}', or 'if !cond {jump}' when 'unless' is set, for a
// jump (a break, continue or return) that is the block's only statement
static IfNode *parseJumpIf(INode *condexp, int unless, INode *jump) {
    BlockNode *ifblk = newBlockNode();
    inodeLexCopy((INode*)ifblk, condexp);
    nodesAdd(&ifblk->stmts, jump);
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

// This helper routine inserts 'break if !condexp' at beginning of block.
// Given the loop's 'else' (parseLoopElse), the loop leaves through that instead:
// 'if !condexp {...else...}', ending in the break that carries the value out.
void parseInsertWhileBreak(INode *blk, INode *condexp, BlockNode *elseblk) {
    BlockNode *loop = (BlockNode*)blk;
    if (elseblk == NULL) {
        nodesInsert(&loop->stmts, (INode*)parseBreakIf(condexp, 1, NULL), 0);
        return;
    }
    LogicNode *notcond = newLogicNode(NotLogicTag);
    inodeLexCopy((INode*)notcond, condexp);
    notcond->lexp = condexp;
    IfNode *ifnode = newIfNode();
    inodeLexCopy((INode*)ifnode, condexp);
    nodesAdd(&ifnode->condblk, (INode*)notcond);
    nodesAdd(&ifnode->condblk, (INode*)blockElseFinish(elseblk, loop, condexp));
    nodesInsert(&loop->stmts, (INode*)ifnode, 0);
}

// A loop's 'else', after its body: the block that says what the loop gives when it
// runs out. NULL where the loop has none. The block is finished (blockElseFinish)
// when the loop's exit is built.
static BlockNode *parseLoopElse(ParseState *parse) {
    if (!lexIsToken(ElseToken))
        return NULL;
    lexNextToken();
    return (BlockNode*)parseExprBlock(parse, 0);
}

// A header's filter, 'if cond', after the source of an 'each' (or the loop in
// '<- each'): the condition, or NULL where there is none. Items for which it does
// not hold are skipped.
INode *parseEachFilter(ParseState *parse) {
    if (!lexIsToken(IfToken))
        return NULL;
    lexNextToken();
    return parseSimpleExpr(parse);
}

// The filter as the statement that runs first in every pass of the loop's body:
// 'if !cond {continue}'. 'lifesym' is the loop's label, if it has one.
INode *parseEachFilterStmt(INode *condexp, Name *lifesym) {
    BreakRetNode *contnode = newContinueNode();
    inodeLexCopy((INode*)contnode, condexp);
    if (lifesym)
        contnode->life = (INode*)newNameUseFromLex(lifesym, condexp);
    return (INode*)parseJumpIf(condexp, 1, (INode*)contnode);
}

// The value of a 'break' or 'return': nil where there is none, the expression
// otherwise. Where an 'if' stands right after the word, it is a value only if its
// condition is followed by a block ('return if c {1} else {2};'); followed by the
// end of the statement ('return if done;') it is the statement's trailing 'if',
// and the condition is left in *trailingp (parseIf reads it so).
// [Ruling: a value that is an 'if' expression must be parenthesized. Refusing the
// bare one would break ~40 existing sites; it is read as the 'if' it is, both ways
// being unambiguous from what follows the condition.]
static INode *parseJumpValue(ParseState *parse, INode **trailingp) {
    *trailingp = NULL;
    if (parseIsEndOfStatement())
        return (INode*)newNilLitNode();
    if (!lexIsToken(IfToken))
        return parseAnyExpr(parse);
    parseTrailingIfOut = trailingp;
    INode *value = parseAnyExpr(parse);
    parseTrailingIfOut = NULL;
    return *trailingp ? (INode*)newNilLitNode() : value;
}

// The end of a 'break', 'continue' or 'return': an 'if' after what it says makes
// it conditional, 'continue if n % 2 == 0;' being 'if n % 2 == 0 {continue}'.
// Nothing but these three takes one. 'trailing' is the condition where
// parseJumpValue found it.
static INode *parseJumpEnd(ParseState *parse, BreakRetNode *jump, INode *trailing) {
    if (trailing == NULL && lexIsToken(IfToken)) {
        lexNextToken();
        trailing = parseSimpleExpr(parse);
    }
    parseEndOfStatement();
    if (trailing == NULL)
        return (INode*)jump;
    return (INode*)parseJumpIf(trailing, 0, (INode*)jump);
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
    // In a generator it ends the walk: the caller is handed None from then on
    if (parse->genctx) {
        if (!parseIsEndOfStatement() && !lexIsToken(IfToken)) {
            errorMsgLex(ErrorYieldReturn,
                "A generator hands its caller values with 'yield', so its 'return' takes none: it ends the walk.");
            parseAnyExpr(parse);
        }
        stmtnode->exp = parseGenNone(parse);
        return parseJumpEnd(parse, stmtnode, NULL);
    }
    INode *trailing;
    stmtnode->exp = parseJumpValue(parse, &trailing);
    return parseJumpEnd(parse, stmtnode, trailing);
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
    INode **trailingp = parseTrailingIfOut;
    parseTrailingIfOut = NULL;
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
        INode *cond = parseSimpleExpr(parse);
        // 'return if done;': the 'if' is the statement's, with nothing to run
        if (trailingp != NULL && cond != NULL && parseIsEndOfStatement()) {
            *trailingp = cond;
            return retnode;
        }
        nodesAdd(&ifnode->condblk, cond);
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

// Finish a range pattern, whose lower bound is parsed and whose '..' or '..<'
// is the current token. It tests the matched value against both bounds:
// 'a .. b' is 'v >= a and v <= b', and 'a ..< b' is 'v >= a and v < b'.
static INode *parseMatchRange(ParseState *parse, INode *matchee, INode *lower) {
    // All three nodes take the operator's position, so a matched value that
    // cannot be ordered is reported at the '..'
    if (lexIsToken(EllipsisToken))
        parseRangeEllipsis();
    FnCallNode *gecall = newFnCallOp(matchee, ">=", 2);
    FnCallNode *upcall = newFnCallOp(matchee, lexIsToken(DotDotLessToken) ? "<" : "<=", 2);
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
// - 'a .. b' or 'a ..< b' is a range (parseMatchRange)
// - a value alone, '2', '"s"', 'K', means equality with the matched value; a
//   bare name is asked of the matched value's enum first, so the test waits for
//   type check to become '==' or 'is' (newMatchValueNode)
// A condition, 'not b' or 'n > 3', is refused (ErrorPatBare): as a value alone it
// would be compared with the matched value, and whether it should be is not
// decided. It is parsed to its end, its own 'or' included, so that it is
// reported once.
#define PatBareMsg "A condition is not a pattern: a value alone is compared with the matched value. Write the comparison with the matched value left out, as '>3', or put the condition in an 'if' guard."
static INode *parseMatchPattern(ParseState *parse, INode *matchee) {
    if (lexIsToken(IsToken)) {
        CastNode *isnode = newIsNode(matchee, unknownType);
        lexNextToken();
        isnode->typ = parseTypeReq(parse, "'is'");
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
    if (lexIsToken(NotToken)) {
        INode *cond = parseSimpleExpr(parse);
        errorMsgNode(cond, ErrorPatBare, PatBareMsg);
        return cond;
    }
    INode *value = parseOr(parse);
    if (lexIsRangeOp())
        return parseMatchRange(parse, matchee, value);
    if (parseCmpOp() != NULL || lexIsToken(IsToken) || lexIsToken(AndToken)) {
        errorMsgNode(value, ErrorPatBare, PatBareMsg);
        return parseSimpleExprFrom(parse, value);
    }
    return (INode *)newMatchValueNode(matchee, value);
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

            // Anything else is one or more patterns joined by 'or', then an
            // optional 'if' guard
            INode *cond = parseMatchPattern(parse, (INode *)expnamenode);
            while (lexIsToken(OrToken)) {
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
    if (!parseHasBlock())
        condexp = parseSimpleExpr(parse);
    BlockNode *loopnode = (BlockNode*)parseExprBlock(parse, 1);
    loopnode->lifesym = lifesym;
    BlockNode *elseblk = parseLoopElse(parse);
    if (condexp) {
        // A loop with a condition has an exit that gives no value, which is the
        // 'else' it needs to be used as one
        if (!stmtflag && elseblk == NULL)
            errorMsgNode((INode*)loopnode, ErrorNoLoop, "while with condition expression may not be used as an expression unless it has an 'else'");
        parseInsertWhileBreak((INode*)loopnode, condexp, elseblk);
    }
    else if (elseblk)
        errorMsgNode((INode*)elseblk, ErrorLoopElse, "A 'while' with no condition never runs out, so it takes no 'else': every 'break' gives its value");
    return (INode *)loopnode;
}

// Set while parseExprBlock hands a 'parallel each' to parseEach, which reads and
// clears it before it parses anything else, so that an 'each' in the body is
// not taken for a parallel one
static int parseEachParallel = 0;

// A source that is a name or fields named from one ('xs', 'self.items'), spelled as
// written into 'out'; 0 for any other expression, whose spelling a message cannot repeat
static int parseEachSpelling(INode *node, char *out, size_t size) {
    if (node->tag == NameUseTag) {
        Name *name = ((NameUseNode*)node)->namesym;
        if (name == NULL || (size_t)name->namesz + 1 > size)
            return 0;
        memcpy(out, &name->namestr, name->namesz);
        out[name->namesz] = '\0';
        return 1;
    }
    if (node->tag == FnCallTag && ((FnCallNode*)node)->args == NULL
        && ((FnCallNode*)node)->methfld && ((FnCallNode*)node)->methfld->tag == NameUseTag) {
        FnCallNode *field = (FnCallNode*)node;
        Name *name = ((NameUseNode*)field->methfld)->namesym;
        if (!parseEachSpelling(field->objfn, out, size))
            return 0;
        size_t used = strlen(out);
        if (name == NULL || used + 1 + name->namesz + 1 > size)
            return 0;
        out[used] = '.';
        memcpy(out + used + 1, &name->namestr, name->namesz);
        out[used + 1 + name->namesz] = '\0';
        return 1;
    }
    return 0;
}

// The source of an 'each' or 'parallel each' written '&mut src' is refused: 'each' reads
// the items of what it walks, and the way to change them in place is a method that
// lends them, 'src.mutItems()'
static void parseEachMutSource(INode *iter, int parallel) {
    if (iter->tag != RefTag || ((RefNode*)iter)->perm->tag == UnknownTag
        || itypeGetTypeDcl(((RefNode*)iter)->perm) != (INode*)mutPerm)
        return;
    char spelling[96];
    INode *src = ((RefNode*)iter)->vtexp;
    if (src == NULL || !parseEachSpelling(src, spelling, sizeof(spelling)))
        strcpy(spelling, "src");
    errorMsgNode(iter, ErrorEachMutSource,
        "%s over '&mut %s' is refused: it reads the items of what it walks. To change the items in place, write '%s.mutItems()'.",
        parallel ? "A 'parallel each'" : "An 'each'", spelling, spelling);
}

// The variable of a drain, '<- each src', which no name reaches
static Name *parseDrainName() {
    static Name *name = NULL;
    if (name == NULL)
        name = nametblFind("-item", 5);
    return name;
}

// What the loop of an each entry of '<-' appends each pass, in place of a body
// in braces (EachBuild says which entry it is):
// - 'yield v' or 'yield k: v', after the header, for 'each x in src [if c] yield v';
// - the pass variable itself, for a drain, 'each src'
// The loop's last statement is the entry that lowers to the append (yieldEntryLower).
static BlockNode *parseEachYield(ParseState *parse, int build, INode *lexnode) {
    BlockNode *loopnode = newLoopBlockNode();
    if (loopnode->stmts == NULL)
        loopnode->stmts = newNodes(8);
    EntryNode *entry = newEntryNode(YieldEntryTag, NULL);
    inodeLexCopy((INode*)entry, lexnode);
    if (build == EachBuildDrain) {
        NameUseNode *item = newNameUseNode(parseDrainName());
        inodeLexCopy((INode*)item, lexnode);
        entry->val = (INode*)item;
        entry->drain = 1;
    }
    else {
        if (!lexIsToken(YieldToken)) {
            errorMsgLex(ErrorEachEntry, "An 'each' with variables inside '<-' gives the value to append with 'yield', after its source and any 'if': 'xs <- each x in ys if *x > 0 yield *x'.");
            if (lexIsToken(LCurlyToken))    // a body written out is read and dropped, so that it is not reported again
                parseExprBlock(parse, 1);
            return NULL;
        }
        lexNextToken();
        if (parseIsEndOfStatement() || lexIsToken(CommaToken) || lexIsToken(RParenToken)) {
            errorMsgLex(ErrorEachEntry, "'yield' needs the value to append.");
            return NULL;
        }
        INode *val = parseSimpleExpr(parse);
        if (val == NULL)
            return NULL;
        if (lexIsToken(ColonToken)) {
            lexNextToken();
            entry->first = val;
            val = parseSimpleExpr(parse);
            if (val == NULL)
                return NULL;
        }
        entry->val = val;
    }
    nodesAdd(&loopnode->stmts, (INode*)entry);
    return loopnode;
}

// Which way a literal step moves a range: 1 up, -1 down, 0 for a step of zero
// (the range runs no pass); 2 for a step that is not a literal, whose sign is
// only known when the loop runs
static int parseRangeStepSign(INode *step) {
    if (step->tag == ULitTag && !(step->flags & FlagCharLit)) {
        if (((ULitNode*)step)->uintlit == 0)
            return 0;
        return (step->flags & FlagLitNeg) ? -1 : 1;
    }
    if (step->tag == FLitTag) {
        double v = ((FLitNode*)step)->floatlit;
        return v > 0 ? 1 : v < 0 ? -1 : 0;
    }
    return 2;
}

// 'by > by - by' or 'by < by - by', the held step's sign: zero is written as the
// step minus itself, which is zero in the step's own type, a float's included
static VarDclNode *parseRangeSign(VarDclNode *by, int up, INode *lexnode) {
    FnCallNode *zero = newFnCallOpnameLower(lexnode, parseEachCounterUse(by, lexnode), minusName, 1);
    nodesAdd(&zero->args, parseEachCounterUse(by, lexnode));
    FnCallNode *cmp = newFnCallOpnameLower(lexnode, parseEachCounterUse(by, lexnode), up ? gtName : ltName, 1);
    nodesAdd(&cmp->args, (INode*)zero);
    VarDclNode *flag = newVarDclFull(nametblFind(up ? "-up" : "-down", up ? 3 : 5), VarDclTag, unknownType,
        (INode*)immPerm, (INode*)cmp);
    inodeLexCopy((INode*)flag, lexnode);
    return flag;
}

// A use of one of a held step's two signs; a step that is the literal 0 has neither
static INode *parseRangeFlag(VarDclNode *flag, INode *lexnode) {
    if (flag)
        return parseEachCounterUse(flag, lexnode);
    INode *no = (INode*)newULitNode(0, (INode*)boolType);
    inodeLexCopy(no, lexnode);
    return no;
}

// 'a and b' or 'a or b' of two conditions
static INode *parseRangeLogic(uint16_t tag, INode *left, INode *right, INode *lexnode) {
    LogicNode *logic = newLogicNode(tag);
    inodeLexCopy((INode*)logic, lexnode);
    logic->lexp = left;
    logic->rexp = right;
    return (INode*)logic;
}

// A use of a range's end. An end that is not a literal is held in a variable of the
// loop's, read once before the first pass, and each use is a use of that variable; a
// literal is cloned for each use (a node reachable twice in the tree is type checked
// twice), and takes the counter's type where it is compared with it
static INode *parseRangeEndUse(VarDclNode *enddcl, INode *endlit, INode *lexnode) {
    if (enddcl)
        return parseEachCounterUse(enddcl, lexnode);
    CloneState cstate = {0};     // Every field the clone reads, the ones not set below NULL
    cstate.instnode = NULL;
    cstate.selftype = NULL;
    cstate.selfparm = NULL;
    cstate.scope = 0;
    return cloneNode(&cstate, endlit);
}

// The test that lets a pass of a range run: the counter has not passed the end
// in the range's direction ('<=' or '<' going up, '>=' or '>' going down, the end
// itself passing for '..'). Where the direction is the held step's sign, either way:
// '(up and x <= end) or (down and x >= end)'
static INode *parseRangeGuard(INode *lexnode, VarDclNode *counter, VarDclNode *enddcl, INode *endlit, int incl, int dir,
    VarDclNode *up, VarDclNode *down) {
    if (dir != 0) {
        FnCallNode *cmp = newFnCallOpnameLower(lexnode, parseEachCounterUse(counter, lexnode),
            dir > 0 ? (incl ? leName : ltName) : (incl ? geName : gtName), 1);
        nodesAdd(&cmp->args, parseRangeEndUse(enddcl, endlit, lexnode));
        return (INode*)cmp;
    }
    FnCallNode *upcmp = newFnCallOpnameLower(lexnode, parseEachCounterUse(counter, lexnode), incl ? leName : ltName, 1);
    nodesAdd(&upcmp->args, parseRangeEndUse(enddcl, endlit, lexnode));
    FnCallNode *downcmp = newFnCallOpnameLower(lexnode, parseEachCounterUse(counter, lexnode), incl ? geName : gtName, 1);
    nodesAdd(&downcmp->args, parseRangeEndUse(enddcl, endlit, lexnode));
    return parseRangeLogic(OrLogicTag,
        parseRangeLogic(AndLogicTag, parseRangeFlag(up, lexnode), (INode*)upcmp, lexnode),
        parseRangeLogic(AndLogicTag, parseRangeFlag(down, lexnode), (INode*)downcmp, lexnode), lexnode);
}

// The test that a step wrapped the counter past its type's extreme: it moved
// against the range's direction, 'x < prev' going up and 'x > prev' going down;
// for a held step, whichever its sign says
static INode *parseRangeMoved(INode *lexnode, VarDclNode *counter, VarDclNode *prev, int dir,
    VarDclNode *up, VarDclNode *down) {
    FnCallNode *lower = NULL, *higher = NULL;
    if (dir >= 0) {
        lower = newFnCallOpnameLower(lexnode, parseEachCounterUse(counter, lexnode), ltName, 1);
        nodesAdd(&lower->args, parseEachCounterUse(prev, lexnode));
    }
    if (dir <= 0) {
        higher = newFnCallOpnameLower(lexnode, parseEachCounterUse(counter, lexnode), gtName, 1);
        nodesAdd(&higher->args, parseEachCounterUse(prev, lexnode));
    }
    if (dir > 0)
        return (INode*)lower;
    if (dir < 0)
        return (INode*)higher;
    return parseRangeLogic(OrLogicTag,
        parseRangeLogic(AndLogicTag, parseRangeFlag(up, lexnode), (INode*)lower, lexnode),
        parseRangeLogic(AndLogicTag, parseRangeFlag(down, lexnode), (INode*)higher, lexnode), lexnode);
}

// 'n += 1', the count of steps a range has taken
static INode *parseRangeCount(VarDclNode *count, INode *lexnode) {
    FnCallNode *add = newFnCallOpnameLower(lexnode, parseEachCounterUse(count, lexnode), plusEqName, 1);
    add->flags |= FlagOpAssgn | FlagLvalOp;
    INode *one = (INode*)newULitNode(1, (INode*)usizeType);
    inodeLexCopy(one, lexnode);
    nodesAdd(&add->args, one);
    return (INode*)add;
}

// 'f64.from(n)': the count of steps as the float type a range's counter may be
static INode *parseRangeConvert(char *type, size_t len, VarDclNode *count, INode *lexnode) {
    FnCallNode *call = newFnCallLower(lexnode, (INode*)newNameUseFromLex(nametblFind(type, len), lexnode), 1);
    call->methfld = (INode*)newMemberUseNode(fromName);
    inodeLexCopy(call->methfld, lexnode);
    nodesAdd(&call->args, parseEachCounterUse(count, lexnode));
    return (INode*)call;
}

// The step of a range's counter, which type check finishes once it knows the
// counter's type (eachRangeStepLower): 'x += step', or 'x++' with no step, or for
// a float 'x = first + n * step'. Its arguments are the first value, the count, the
// count converted to f32 and to f64, and the step when there is one.
static INode *parseRangeStepCall(INode *lexnode, VarDclNode *counter, VarDclNode *first, VarDclNode *count, INode *step) {
    FnCallNode *call = newFnCallOpnameLower(lexnode, parseEachCounterUse(counter, lexnode), eachRangeStepName, step ? 5 : 4);
    call->flags |= FlagLvalOp;
    if (step)
        call->flags |= FlagOpAssgn;
    nodesAdd(&call->args, parseEachCounterUse(first, lexnode));
    nodesAdd(&call->args, parseEachCounterUse(count, lexnode));
    nodesAdd(&call->args, parseRangeConvert("f32", 3, count, lexnode));
    nodesAdd(&call->args, parseRangeConvert("f64", 3, count, lexnode));
    if (step)
        nodesAdd(&call->args, step);
    return (INode*)call;
}

// Parse each block. 'build' is EachBuildNone for an each statement or expression,
// and for an entry of '<-' EachBuildVars ('each x in src [if c] yield v') or
// EachBuildDrain ('each src'), whose body is the yield (parseEachYield).
static INode *parseEachLoop(ParseState *parse, Name *lifesym, int stmtflag, int build) {
    int parallel = parseEachParallel;
    parseEachParallel = 0;
    // In an actor's body, a 'parallel each' may be cut like an 'await' (a seam,
    // pareach.c), so the method holding one is noted as holding one: its actor
    // is given a pending table and the rest a seam needs (parseactor.c)
    if (parallel && parse->dcltexts)
        ++parse->dcltexts->awaits;
    BlockNode *outerblk = newBlockNode();   // surrounding block scope for isolating 'each' vars

    // Obtain all the parsed pieces
    lexNextToken();
    VarDclNode *elemvars[EachMaxVars];
    uint32_t nelems = 0;
    if (build == EachBuildDrain) {
        elemvars[nelems++] = newVarDclNode(parseDrainName(), VarDclTag, (INode*)immPerm);
    }
    else {
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorNoVar, "Missing variable name");
        return (INode *)outerblk;
    }
    // One variable, or several that unpack a tuple item: 'each k, v in dict'
    // Each is a fresh variable of every pass that nothing can change; a pass's
    // value is given it once the loop is built
    while (1) {
        if (nelems == EachMaxVars) {
            errorMsgLex(ErrorBadTok, "An 'each' unpacks at most %d variables", EachMaxVars);
            return (INode *)outerblk;
        }
        elemvars[nelems++] = newVarDclNode(lex->val.ident, VarDclTag, (INode*)immPerm);
        lexNextToken();
        if (!lexIsToken(CommaToken))
            break;
        lexNextToken();
        if (!lexIsToken(IdentToken)) {
            errorMsgLex(ErrorNoVar, "Missing variable name");
            return (INode *)outerblk;
        }
    }
    if (!lexIsToken(InToken)) {
        errorMsgLex(ErrorBadTok, "Missing 'in'");
        return (INode *)outerblk;
    }
    lexNextToken();
    }
    INode *iter = parseSimpleExpr(parse);
    if (iter == NULL)       // Not a term, and already reported as such
        return (INode *)outerblk;
    // A number range: 'a .. b' runs through b, 'a ..< b' stops before it, and a
    // step after 'by' moves it by that much (a negative step counts down)
    INode *rangeend = NULL;
    int rangeincl = 0;
    if (lexIsRangeOp()) {
        if (lexIsToken(EllipsisToken))
            parseRangeEllipsis();
        rangeincl = !lexIsToken(DotDotLessToken);
        lexNextToken();
        rangeend = parseSimpleExpr(parse);
        if (rangeend == NULL)       // Not a term, and already reported as such
            return (INode *)outerblk;
    }
    else if (iter->tag == FnCallTag && ((FnCallNode*)iter)->methfld
        && (iter->flags & FlagOperator) && ((FnCallNode*)iter)->args && ((FnCallNode*)iter)->args->used == 1) {
        // The range this was once written as, a comparison, is refused. It is read on as
        // the range it stands for, so that the rest of the loop is not reported again
        FnCallNode *old = (FnCallNode*)iter;
        Name *methodnm = ((NameUseNode*)old->methfld)->namesym;
        char *oldop = methodnm == ltName ? "<" : methodnm == leName ? "<=" : methodnm == gtName ? ">" : methodnm == geName ? ">=" : NULL;
        if (oldop != NULL) {
            int up = methodnm == ltName || methodnm == leName;
            int incl = methodnm == leName || methodnm == geName;
            errorMsgNode(iter, ErrorEachCompare,
                "A range is written with '..' or '..<', not as a comparison: 'each i in a %s b' is 'each i in a %s b%s'. '..' runs through its end and '..<' stops before it; counting down takes a negative step.",
                oldop, incl ? ".." : "..<", up ? "" : " by -1");
            rangeincl = incl;
            rangeend = nodesGet(old->args, 0);
            iter = old->objfn;
        }
    }
    int isrange = rangeend != NULL;
    if (!isrange)
        parseEachMutSource(iter, parallel);
    INode *step = NULL;
    if (isrange && lexIsToken(ByToken)) {
        lexNextToken();
        step = parseSimpleExpr(parse);
        if (step == NULL)
            return (INode *)outerblk;
    }
    INode *filter = build == EachBuildDrain ? NULL : parseEachFilter(parse);
    BlockNode *loopnode;
    BlockNode *elseblk = NULL;
    if (build != EachBuildNone) {
        // An entry's loop appends what it yields and gives no value of its own
        loopnode = parseEachYield(parse, build, iter);
        if (loopnode == NULL)
            return (INode *)outerblk;
        loopnode->lifesym = lifesym;
    }
    else {
        loopnode = (BlockNode*)parseExprBlock(parse, 1);
        loopnode->lifesym = lifesym;
        // An 'else' says what the loop gives when it runs out, so that the loop can be
        // used as a value. Without one a loop gives none, and is a statement.
        elseblk = parseLoopElse(parse);
        if (!stmtflag && elseblk == NULL)
            errorMsgNode((INode*)loopnode, ErrorNoLoop, "each may not be used as an expression unless it has an 'else'");
    }

    // The passes of a parallel each run at the same time and give no value, so
    // there is no 'else' for it to run out into
    if (parallel && elseblk) {
        errorMsgNode((INode*)elseblk, ErrorParElse,
            "A 'parallel each' takes no 'else': its passes run at the same time and give no value, so there is no running out for it to say what happens at.");
        return (INode *)outerblk;
    }

    // A parallel range counts up by one from the first bound to the second,
    // each pass a number; the hidden variables hold the bounds, type check
    // builds the loop (parallelEachLower) once it knows their type
    if (parallel && isrange) {
        if (nelems != 1) {
            errorMsgNode(iter, ErrorBadTok, "A numeric range gives one variable, the number.");
            return (INode *)outerblk;
        }
        if (step) {
            errorMsgNode(iter, ErrorParSource,
                "A 'parallel each' over a number range counts up by one ('parallel each i in 0 ..< n', or '..'): which pass runs first is not defined in parallel, so a count down or a step is not offered.");
            return (INode *)outerblk;
        }
        parallelEachNames();
        VarDclNode *firstdcl = newVarDclFull(parFirstName, VarDclTag, unknownType, (INode*)immPerm, iter);
        inodeLexCopy((INode*)firstdcl, iter);
        VarDclNode *lastdcl = newVarDclFull(parLastName, VarDclTag, unknownType, (INode*)immPerm, rangeend);
        inodeLexCopy((INode*)lastdcl, iter);
        outerblk->flags |= FlagEach | FlagParallel;
        if (rangeincl)
            outerblk->flags |= FlagParIncl;
        nodesAdd(&outerblk->stmts, (INode*)firstdcl);
        nodesAdd(&outerblk->stmts, (INode*)lastdcl);
        nodesInsert(&loopnode->stmts, (INode*)elemvars[0], 0);
        if (filter)
            nodesInsert(&loopnode->stmts, parseEachFilterStmt(filter, lifesym), 1);
        nodesAdd(&outerblk->stmts, (INode*)loopnode);
        return (INode *)outerblk;
    }

    // Assemble logic for a range (with optional step), e.g. 'each x in a .. b by s':
    // { imm first = a; mut counter = first; mut n = 0;
    //   while counter <= b { imm x = counter; ... ; { n += 1; counter += s } } }
    // The loop's variable is a new one on every pass, which the body cannot
    // change: counting by hand is a 'while'. The counter is the loop's own, a
    // variable no name reaches, its uses bound here. The first value and the count
    // of steps taken are kept for a float counter, whose step is computed from them
    // (eachRangeStepLower).
    if (isrange) {
        if (nelems != 1) {
            errorMsgNode(iter, ErrorBadTok, "A numeric range gives one variable, the number.");
            return (INode *)outerblk;
        }
        eachRangeNames();
        // Every node below is built after parseExprBlock has consumed the whole
        // loop body, so the lexer sits on the token following the body's '}' --
        // which is usually the enclosing function's. Position them on the range
        // expression instead, since that is what the reader wrote and what the
        // diagnostic is really about.
        VarDclNode *firstdcl = newVarDclFull(nametblFind("-first", 6), VarDclTag, unknownType, (INode*)immPerm, iter);
        inodeLexCopy((INode*)firstdcl, iter);
        nodesAdd(&outerblk->stmts, (INode*)firstdcl);

        // An end that is not a literal is read once, here, before the first pass (a
        // literal is cloned where it is compared, to take the counter's type)
        VarDclNode *enddcl = NULL;
        if (parseRangeStepSign(rangeend) == 2) {
            enddcl = newVarDclFull(nametblFind("-end", 4), VarDclTag, unknownType, (INode*)immPerm, rangeend);
            inodeLexCopy((INode*)enddcl, iter);
            nodesAdd(&outerblk->stmts, (INode*)enddcl);
        }

        // Which way the range runs. With no step it counts up. A literal step says
        // which by its sign, and a step of 0 runs no pass. Any other step is held in
        // a variable of the loop's (evaluated once, before the first pass) and says
        // by its sign when the loop runs: 'up' and 'down' are what it was.
        int dir = 1;                    // 1 up, -1 down, 0 up or down by the step held
        VarDclNode *updcl = NULL, *downdcl = NULL;
        INode *stepexp = step;          // what the step adds: the literal, or a use of the held step
        if (step) {
            dir = parseRangeStepSign(step);
            if (dir == 2) {
                dir = 0;
                VarDclNode *bydcl = newVarDclFull(nametblFind("-by", 3), VarDclTag, unknownType, (INode*)immPerm, step);
                inodeLexCopy((INode*)bydcl, iter);
                nodesAdd(&outerblk->stmts, (INode*)bydcl);
                updcl = parseRangeSign(bydcl, 1, iter);
                downdcl = parseRangeSign(bydcl, 0, iter);
                nodesAdd(&outerblk->stmts, (INode*)updcl);
                nodesAdd(&outerblk->stmts, (INode*)downdcl);
                stepexp = parseEachCounterUse(bydcl, iter);
            }
        }

        // (a name of its own, since a float counter is assigned and an assignment to the
        // anonymous name discards its value)
        VarDclNode *elemdcl = newVarDclNode(nametblFind("-counter", 8), VarDclTag, (INode*)mutPerm);
        inodeLexCopy((INode*)elemdcl, iter);
        elemdcl->value = parseEachCounterUse(firstdcl, iter);
        nodesAdd(&((BlockNode*)outerblk)->stmts, (INode*)elemdcl);
        VarDclNode *ndcl = newVarDclFull(nametblFind("-n", 2), VarDclTag, unknownType, (INode*)mutPerm,
            (INode*)newULitNode(0, (INode*)usizeType));
        inodeLexCopy((INode*)ndcl, iter);
        nodesAdd(&outerblk->stmts, (INode*)ndcl);
        // A range that gives a value when it runs out has several places it runs
        // out at (the guard, and the steps that stop a counter wrapping or
        // reaching its bound), but one 'else': so they only say it has, in a flag
        // that the guard looks at first in the next pass
        VarDclNode *donedcl = NULL;
        if (elseblk) {
            // (a name of its own, since the counter already holds the anonymous name in this block)
            donedcl = newVarDclFull(nametblFind("-done", 5), VarDclTag, unknownType, (INode*)mutPerm,
                (INode*)newULitNode(0, (INode*)boolType));
            inodeLexCopy((INode*)donedcl, iter);
            nodesAdd(&outerblk->stmts, (INode*)donedcl);
        }
        // The test that lets a pass run
        INode *guard = parseRangeGuard(iter, elemdcl, enddcl, rangeend, rangeincl, dir, updcl, downdcl);
        if (step) {
            // A step of more than one need never land on the bound, so nothing
            // stops it carrying the loop variable past the type's extreme: it
            // wraps to the other end, satisfies the comparison again, and the
            // loop never ends. The bound cannot be consulted ahead of the step
            // to see that coming -- the distance to it overflows on a signed
            // range wider than half its type, and whether the step adds or
            // subtracts is not known until it has been evaluated -- but the wrap
            // is plain afterwards: the loop variable moved against the range's
            // direction. So the step is '{ imm prev = x; x += s; if x < prev
            // {break} }', with '>' for a range counting down. The statements stay one
            // statement, because the trailing statement is what a 'continue'
            // carries a copy of, and 'prev' is a phantom variable the copy
            // re-points at its own declaration.
            VarDclNode *prevdcl = newVarDclFull(anonName, VarDclTag, unknownType, (INode*)immPerm,
                parseEachCounterUse(elemdcl, iter));
            inodeLexCopy((INode*)prevdcl, iter);
            INode *wrapped = parseRangeMoved(iter, elemdcl, prevdcl, dir, updcl, downdcl);
            BlockNode *stepblk = newBlockNode();
            inodeLexCopy((INode*)stepblk, iter);
            nodesAdd(&stepblk->stmts, (INode*)prevdcl);
            nodesAdd(&stepblk->stmts, parseRangeCount(ndcl, iter));
            nodesAdd(&stepblk->stmts, parseRangeStepCall(iter, elemdcl, firstdcl, ndcl, stepexp));
            if (donedcl)
                nodesAdd(&stepblk->stmts, (INode*)parseJumpIf(wrapped, 0, parseEachSetDone(donedcl, iter)));
            else
                nodesAdd(&stepblk->stmts, (INode*)parseBreakIf(wrapped, 0, lifesym));
            nodesAdd(&loopnode->stmts, (INode*)stepblk);
        }
        else {
            INode *incr = parseRangeStepCall(iter, elemdcl, firstdcl, ndcl, NULL);
            BlockNode *stepblk = newBlockNode();
            inodeLexCopy((INode*)stepblk, iter);
            nodesAdd(&stepblk->stmts, parseRangeCount(ndcl, iter));
            if (rangeincl) {
                // An inclusive range's last value is its bound, and the bound may
                // be the type's maximum. Stepping past it wraps, the wrapped value
                // passes the guard again, and the loop never ends -- LLVM folds
                // 'x <= MAX' to true and emits a loop with no exit. So the step is
                // guarded: '{ if x == bound {break}; x++ }'.
                // The statements stay one statement, because the trailing statement is
                // what a 'continue' carries a copy of. The bound is a use of its
                // variable, or a clone of its literal, not the guard's own node.
                FnCallNode *atbound = newFnCallOpnameLower(iter, parseEachCounterUse(elemdcl, iter), eqName, 1);
                nodesAdd(&atbound->args, parseRangeEndUse(enddcl, rangeend, iter));
                if (donedcl) {
                    // '{ if x == bound {done = true} else {x++} }'
                    IfNode *atend = parseJumpIf((INode*)atbound, 0, parseEachSetDone(donedcl, iter));
                    BlockNode *stepon = newBlockNode();
                    inodeLexCopy((INode*)stepon, iter);
                    nodesAdd(&stepon->stmts, incr);
                    nodesAdd(&atend->condblk, elseCond);
                    nodesAdd(&atend->condblk, (INode*)stepon);
                    nodesAdd(&stepblk->stmts, (INode*)atend);
                }
                else {
                    nodesAdd(&stepblk->stmts, (INode*)parseBreakIf((INode*)atbound, 0, lifesym));
                    nodesAdd(&stepblk->stmts, incr);
                }
            }
            else
                nodesAdd(&stepblk->stmts, incr);
            nodesAdd(&loopnode->stmts, (INode*)stepblk);
        }
        // The step is now the block's last statement and stays there: the guard
        // below is inserted at index 0, blockTypeCheck appends no 'blockret' to a
        // loop block, and blockFlow runs later. Saying so is what lets name
        // resolution find the step to copy ahead of a 'continue'.
        loopnode->flags |= FlagLoopStep;
        if (donedcl) {
            // 'if done or !(guard) {...else...}'
            LogicNode *notiter = newLogicNode(NotLogicTag);
            inodeLexCopy((INode*)notiter, iter);
            notiter->lexp = guard;
            LogicNode *either = newLogicNode(OrLogicTag);
            inodeLexCopy((INode*)either, iter);
            either->lexp = parseEachCounterUse(donedcl, iter);
            either->rexp = (INode*)notiter;
            IfNode *leave = newIfNode();
            inodeLexCopy((INode*)leave, iter);
            nodesAdd(&leave->condblk, (INode*)either);
            nodesAdd(&leave->condblk, (INode*)blockElseFinish(elseblk, loopnode, iter));
            nodesInsert(&loopnode->stmts, (INode*)leave, 0);
        }
        else
            parseInsertWhileBreak((INode*)loopnode, guard, NULL);
        // The pass's variable, taken from the counter after the guard has let the pass run
        elemvars[0]->value = parseEachCounterUse(elemdcl, iter);
        nodesInsert(&loopnode->stmts, (INode*)elemvars[0], 1);
        if (filter)
            nodesInsert(&loopnode->stmts, parseEachFilterStmt(filter, lifesym), 2);
        nodesAdd(&outerblk->stmts, (INode*)loopnode);
    }
    else {
        // Anything but a range is walked as its type says: type check builds the
        // loop (eachLower) once it knows whether the source is an array or slice,
        // a cursor, or gives one. The source waits in a hidden variable of the
        // block, and the reader's variables are declared, with no value yet, at
        // the head of the loop.
        outerblk->flags |= FlagEach;
        if (parallel)
            outerblk->flags |= FlagParallel;
        VarDclNode *srcdcl = newVarDclFull(anonName, VarDclTag, unknownType, (INode*)mutPerm, iter);
        inodeLexCopy((INode*)srcdcl, iter);
        nodesAdd(&outerblk->stmts, (INode*)srcdcl);
        for (uint32_t i = nelems; i > 0; --i) {
            elemvars[i - 1]->flags |= FlagEachVar;
            nodesInsert(&loopnode->stmts, (INode*)elemvars[i - 1], 0);
        }
        // The filter follows the variables (which hold the pass's item by then);
        // the 'else' stands ahead of them, where it cannot name them (eachLower
        // takes it out to make the loop's exit)
        if (filter)
            nodesInsert(&loopnode->stmts, parseEachFilterStmt(filter, lifesym), nelems);
        if (elseblk)
            nodesInsert(&loopnode->stmts, (INode*)blockElseFinish(elseblk, loopnode, iter), 0);
        nodesAdd(&outerblk->stmts, (INode*)loopnode);
    }
    return (INode *)outerblk;
}

INode *parseEach(ParseState *parse, Name *lifesym, int stmtflag) {
    return parseEachLoop(parse, lifesym, stmtflag, EachBuildNone);
}

// An entry of '<-' that begins with 'each': a loop whose body is the append of
// what it yields (an EachEntryTag holding it, lowered by the '<-', contentsLower)
INode *parseEachEntry(ParseState *parse) {
    EntryNode *entry = newEntryNode(EachEntryTag, NULL);
    entry->first = parseEachLoop(parse, NULL, 1, lexEachHasVars() ? EachBuildVars : EachBuildDrain);
    return (INode*)entry;
}

// The same entry with 'parallel' in front of its 'each' (the lexer on the 'each'):
// the loop's passes run at the same time and the values they yield are joined in
// the order of the passes (the parallel builder, pareach.c)
INode *parseParallelEachEntry(ParseState *parse) {
    parseEachParallel = 1;
    return parseEachEntry(parse);
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

    // A block's statements stand at the top level, even inside a list: a
    // comma there belongs to them, not to the list around the block
    int svinlist = parse->inlist;
    parse->inlist = 0;
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

        // A generator's seam, 'yield e;' or 'yield each src;'
        case YieldToken:
            nodesAdd(&blk->stmts, parseYield(parse));
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

        // 'parallel' is a word only directly before 'each'; anywhere else it is
        // a name like any other
        case IdentToken:
            if (lex->val.ident == parallelName && lexNextIsWord("each")) {
                lexNextToken();
                parseEachParallel = 1;
                nodesAdd(&blk->stmts, parseEach(parse, NULL, 1));
                break;
            }
            nodesAdd(&blk->stmts, parseExpStmt(parse));
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
            INode *trailing;
            node->exp = parseJumpValue(parse, &trailing);
            nodesAdd(&blk->stmts, parseJumpEnd(parse, node, trailing));
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
            nodesAdd(&blk->stmts, parseJumpEnd(parse, node, NULL));
            break;
        }

        case LCurlyToken:
            nodesAdd(&blk->stmts, parseExprBlock(parse, 0));
            break;

        // 'async do' declares an actor's behaviour, in an actor's body: here
        // it is reported, with either word alone, and the declaration passed
        // over whole
        case AsyncToken:
        case DoToken:
            parseBehaviourWords("a function's body");
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

    parse->inlist = svinlist;
    return (INode*)blk;
}

// Skip what is left of a 'where' clause the parser refused, to the block or
// the end of the declaration
static void parseWhereSkip() {
    while (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken) && !lexIsToken(RCurlyToken)
        && !lexIsToken(EofToken))
        lexNextToken();
}

// Is the condition being parsed one on an entry of a type's 'is' list,
// 'is Move if T is Move', rather than a 'where' clause? The two are read alike;
// only what a refusal calls the condition differs.
static int parseInIsCond = 0;

// 'not' is not built [Jon 27 Sep]: a clause says what a type is
static void parseWhereNot() {
    errorMsgLex(ErrorWhereForm, parseInIsCond
        ? "'not' is not built: a condition on an 'is' entry says what a type parameter is, and clauses are joined by 'and' and 'or'."
        : "'not' is not built: a 'where' clause says what a type parameter is, and clauses are joined by 'and' and 'or'.");
}

// Join two conditions with 'and' or 'or', positioned where the left one is
static INode *parseWhereJoin(int16_t tag, INode *lhs, INode *rhs) {
    LogicNode *join = newLogicNode(tag);
    // A lifetime comparison stands in a condition as a node with no position
    inodeLexCopy((INode*)join, lhs->lexer ? lhs : rhs);
    join->lexp = lhs;
    join->rexp = rhs;
    return (INode*)join;
}

static INode *parseWhereOr(ParseState *parse, LifeOrder **orderp);

// What a lifetime comparison leaves in a condition: it is recorded in the
// order as it is read, and stands in the condition only so that an 'or' or a
// 'not' over it is found and refused
static INode parseLifeClauseNode;
#define parseLifeClause (&parseLifeClauseNode)

// Does a condition hold a lifetime comparison?
static int parseWhereHasLife(INode *cond) {
    if (cond == parseLifeClause)
        return 1;
    if (cond->tag == AndLogicTag || cond->tag == OrLogicTag)
        return parseWhereHasLife(((LogicNode*)cond)->lexp) || parseWhereHasLife(((LogicNode*)cond)->rexp);
    return 0;
}

// A lifetime comparison, with the lexer on its first lifetime: ''a >= 'b'
// (''a' lasts at least as long as ''b') or ''a == 'b' (each as long as the
// other), recorded in '*orderp', the order of the declaration's lifetimes
// (lifetime.h). NULL, reported, where it is malformed or nothing has lifetimes.
static INode *parseWhereLife(ParseState *parse, LifeOrder **orderp) {
    INode *at = (INode*)newNameUseNode(lex->val.ident);
    Name *longer = lex->val.ident;
    lexNextToken();
    int equal = lexIsToken(EqToken);
    if (!equal && !lexIsToken(GeToken)) {
        errorMsgLex(ErrorWhereForm, "A lifetime clause compares two lifetimes: ''a >= 'b', ''a' lasting at least as long as ''b', or ''a == 'b'.");
        return NULL;
    }
    lexNextToken();
    if (!lexIsToken(LifetimeToken)) {
        errorMsgLex(ErrorWhereForm, "A lifetime is compared with a lifetime: ''a >= 'b'.");
        return NULL;
    }
    Name *shorter = lex->val.ident;
    lexNextToken();
    // An invariant lifetime is equal only to itself: it has no order with any
    // other, and is equated only with another invariant one
    if (lifeIsInvariant(longer) || lifeIsInvariant(shorter)) {
        if (!equal) {
            errorMsgNode(at, ErrorLifetimeInvariant, "An invariant lifetime has no order: only ''=a == '=b' compares one, declaring the two one brand.");
            return parseLifeClause;
        }
        if (!lifeIsInvariant(longer) || !lifeIsInvariant(shorter)) {
            errorMsgNode(at, ErrorLifetimeInvariant, "An invariant lifetime is equated only with another invariant one: ''=a == '=b'.");
            return parseLifeClause;
        }
    }
    if (orderp == NULL) {
        errorMsgNode(at, ErrorLifetimeUndeclared, "This declares no lifetimes for a 'where' clause to order.");
        return NULL;
    }
    if (*orderp == NULL)
        *orderp = newLifeOrder();
    lifeOrderAdd(*orderp, longer, shorter, at);
    if (equal)
        lifeOrderAdd(*orderp, shorter, longer, at);
    return parseLifeClause;
}

// Does the 'where' clause being parsed take its type parameters' lifetime
// bounds: a function's does, a generic type's does not yet
static int parseWhereBounds = 0;

// A type parameter's lifetime bound, with the lexer on the lifetime after its
// '+': ''+T' >= ''a' in '*orderp', the declaration's order (lifetime.h,
// "Lifetime bounds"), where 'ok' allows one. Stands in a condition as a
// lifetime comparison does, so that an 'or' or a 'not' over it is refused.
static INode *parseBoundAdd(LifeOrder **orderp, int ok, Name *tparm) {
    INode *at = (INode*)newNameUseNode(lex->val.ident);
    Name *life = lex->val.ident;
    lexNextToken();
    if (!ok || orderp == NULL)
        errorMsgNode(at, ErrorLifetimeBound, "A lifetime bound on a generic type's parameter is not built: a type parameter of a generic function takes one, '[T + 'a]' or 'where T + 'a'.");
    else if (lifeIsInvariant(life))
        errorMsgNode(at, ErrorLifetimeInvariant, "A bound says what the borrows inside a type outlive, and an invariant lifetime has no order to say it with.");
    else {
        if (*orderp == NULL)
            *orderp = newLifeOrder();
        lifeOrderAdd(*orderp, lifeBoundName(tparm), life, at);
        if (life == staticLifeName)
            lifeStaticBoundSeen = 1;
    }
    return parseLifeClause;
}

// One term of a 'where' condition: 'T is Name', a trait joined to another by
// '+' as in the inline form (the two clauses joined by 'and'), a lifetime
// bound, 'T + 'a' or 'T is Name + 'a', a lifetime comparison, or a condition
// in parentheses. NULL, reported, for anything else.
static INode *parseWhereTerm(ParseState *parse, LifeOrder **orderp) {
    if (lexIsToken(LParenToken)) {
        lexNextToken();
        INode *inner = parseWhereOr(parse, orderp);
        if (inner == NULL)
            return NULL;
        if (!lexIsToken(RParenToken)) {
            errorMsgLex(ErrorWhereForm, parseInIsCond
                ? "A '(' in a condition on an 'is' entry is closed by a ')' after the clauses it groups."
                : "A '(' in a 'where' clause is closed by a ')' after the clauses it groups.");
            return NULL;
        }
        lexNextToken();
        return inner;
    }
    // A condition on an 'is' entry decides, per instance, what the instance
    // is, and a lifetime is never instanced, so it decides nothing
    if (parseInIsCond && lexIsToken(LifetimeToken)) {
        errorMsgLex(ErrorWhereForm, "A condition on an 'is' entry asks what a type parameter is, and a lifetime is never instanced, so it decides nothing for an instance.");
        return NULL;
    }
    if (lexIsToken(LifetimeToken))
        return parseWhereLife(parse, orderp);
    if (lexIsToken(NotToken)) {
        // Over a lifetime comparison, 'not' is refused for what it would mean
        if (lexPeekIsLifetime()) {
            errorMsgLex(ErrorLifetimeOr, "A lifetime comparison is never under 'not': a lifetime is never instanced, so the body could rely on nothing it would say.");
            return NULL;
        }
        parseWhereNot();
        return NULL;
    }
    if (!lexIsToken(IdentToken)) {
        errorMsgLex(ErrorWhereForm, parseInIsCond
            ? "A condition on an 'is' entry is a type parameter's name, 'is', and a trait: 'is Move if T is Move'."
            : "A 'where' clause is a type parameter's name, 'is', and a trait: 'where T is Integer'.");
        return NULL;
    }
    INode *subject = (INode*)newNameUseNode(lex->val.ident);
    Name *tparm = lex->val.ident;
    lexNextToken();
    // A lifetime bound alone, 'T + 'a', as in the inline form '[T + 'a]'
    if (lexIsToken(PlusToken) && lexPeekIsLifetime()) {
        INode *term = NULL;
        while (lexIsToken(PlusToken)) {
            lexNextToken();
            if (!lexIsToken(LifetimeToken)) {
                errorMsgLex(ErrorWhereForm, "A type parameter's bounds are lifetimes joined by '+', 'T + 'a + 'b'; its traits are said by 'is', 'T is Trait + 'a'.");
                return NULL;
            }
            INode *bound = parseBoundAdd(orderp, parseWhereBounds, tparm);
            term = term ? parseWhereJoin(AndLogicTag, term, bound) : bound;
        }
        return term;
    }
    if (!lexIsToken(IsToken)) {
        errorMsgLex(ErrorWhereForm, "Only 'T is Name' and a lifetime bound, 'T + 'a', are built: a relation between two parameters and a constraint on a type expression are not yet.");
        return NULL;
    }
    lexNextToken();
    INode *term = NULL;
    while (1) {
        if (lexIsToken(NotToken)) {
            parseWhereNot();
            return NULL;
        }
        // 'T is Trait + 'a': a bound after the traits, as '[T Trait + 'a]'
        // writes it. Alone it is 'T + 'a': T is no lifetime.
        if (lexIsToken(LifetimeToken) && term) {
            INode *bound = parseBoundAdd(orderp, parseWhereBounds, tparm);
            term = parseWhereJoin(AndLogicTag, term, bound);
            if (!lexIsToken(PlusToken))
                return term;
            lexNextToken();
            // A trait after the bound is a clause with its own use of T
            INode *again = (INode*)newNameUseNode(tparm);
            inodeLexCopy(again, subject);
            subject = again;
            continue;
        }
        if (!lexIsToken(IdentToken) && !lexIsToken(FnToken)) {
            errorMsgLex(ErrorWhereForm, lexIsToken(LifetimeToken)
                ? "A lifetime bound alone is written 'T + 'a': what a type parameter 'is' is a trait, named."
                : parseInIsCond ? "What a type parameter 'is' in a condition on an 'is' entry is a trait, named."
                : "What a type parameter 'is' in a 'where' clause is a trait, named, or a function signature, 'where F is fn(a &T) i32'.");
            return NULL;
        }
        // A function signature, written bare, is what the type has a '()' method of
        INode *isrhs = lexIsToken(FnToken) ? parseFnBound(parse) : parseTypeName(parse);
        CastNode *clause = newIsNode(subject, isrhs);
        inodeLexCopy((INode*)clause, subject);
        term = term ? parseWhereJoin(AndLogicTag, term, (INode*)clause) : (INode*)clause;
        if (!lexIsToken(PlusToken))
            return term;
        lexNextToken();
        // 'T is A + B' is two clauses, each with its own use of T
        INode *again = (INode*)newNameUseNode(((NameUseNode*)subject)->namesym);
        inodeLexCopy(again, subject);
        subject = again;
    }
}

// Terms joined by 'and', which binds tighter than 'or', as in an expression
static INode *parseWhereAnd(ParseState *parse, LifeOrder **orderp) {
    INode *lhs = parseWhereTerm(parse, orderp);
    while (lhs && lexIsToken(AndToken)) {
        lexNextToken();
        INode *rhs = parseWhereTerm(parse, orderp);
        lhs = rhs ? parseWhereJoin(AndLogicTag, lhs, rhs) : NULL;
    }
    return lhs;
}

// A whole condition: 'and'-joined terms, joined by 'or'. A lifetime comparison
// may not be an alternative: a lifetime is never instanced, so no instance
// settles which alternative holds, and the body could rely on none of them.
static INode *parseWhereOr(ParseState *parse, LifeOrder **orderp) {
    INode *lhs = parseWhereAnd(parse, orderp);
    while (lhs && lexIsToken(OrToken)) {
        INode *orat = (INode*)newNameUseNode(anonName);
        lexNextToken();
        INode *rhs = parseWhereAnd(parse, orderp);
        if (rhs && (parseWhereHasLife(lhs) || parseWhereHasLife(rhs))) {
            errorMsgNode(orat, ErrorLifetimeOr,
                "A lifetime comparison is joined to the rest of a 'where' clause by 'and' only: a lifetime is never instanced, so an 'or' would leave the body nothing it could rely on.");
            return NULL;
        }
        lhs = rhs ? parseWhereJoin(OrLogicTag, lhs, rhs) : NULL;
    }
    return lhs;
}

// Append a condition to a 'where' list, each operand of an 'and' at its top
// an element of its own, in the order written. A lifetime comparison is in
// the order already, and is no element.
static void parseWhereAdd(Nodes **wherep, INode *cond) {
    if (cond == parseLifeClause)
        return;
    if (cond->tag == AndLogicTag) {
        parseWhereAdd(wherep, ((LogicNode*)cond)->lexp);
        parseWhereAdd(wherep, ((LogicNode*)cond)->rexp);
    }
    else {
        if (*wherep == NULL)
            *wherep = newNodes(4);
        nodesAdd(wherep, cond);
    }
}

// Parse a 'where' clause, with the lexer on 'where', appending its condition
// to '*wherep' (generic.h), left NULL where it holds no clause about a type:
// 'T is Name', a trait joined to another by '+' as in the inline form,
// clauses joined by 'and' and 'or', 'and' binding tighter, and parentheses
// grouping. A lifetime comparison, ''a >= 'b' or ''a == 'b', goes to
// '*orderp' instead (lifetime.h), NULL where the declaration has no lifetimes
// to order. What else the manual shows a clause saying -- a relation between
// two type parameters, 'T < Y', and a constraint on a type expression,
// 'Option[T] is Node' -- and 'not' are not built, and are refused here: a
// refused clause adds nothing, and the rest of it is skipped. A type
// parameter's lifetime bound, 'T + 'a' or 'T is Name + 'a', goes to
// '*orderp' as ''+T' >= ''a' where 'bounds' allows one (lifetime.h,
// "Lifetime bounds"), and stands in the condition as a comparison does.
void parseWhere(ParseState *parse, Nodes **wherep, LifeOrder **orderp, int bounds) {
    lexNextToken();  // past 'where'
    parseWhereBounds = bounds;
    INode *cond = parseWhereOr(parse, orderp);
    parseWhereBounds = 0;
    if (cond == NULL) {
        parseWhereSkip();
        return;
    }
    parseWhereAdd(wherep, cond);
}

// Parse the condition on one entry of a type's 'is' list, with the lexer on
// its 'if': 'is Move if T is Move'. It is read as a 'where' clause's condition
// is -- 'T is Name', '+'-joined traits, clauses joined by 'and' and 'or',
// grouped by parentheses -- and kept whole, since it decides one entry rather
// than being a list of requirements. No lifetime takes part. NULL, reported,
// where it is refused; what is left of it is then skipped to the next entry,
// the type's 'where' or its block.
INode *parseIsCondition(ParseState *parse) {
    lexNextToken();  // past 'if'
    parseInIsCond = 1;
    INode *cond = parseWhereOr(parse, NULL);
    parseInIsCond = 0;
    if (cond == NULL) {
        while (!lexIsToken(LCurlyToken) && !lexIsToken(SemiToken) && !lexIsToken(RCurlyToken)
            && !lexIsToken(CommaToken) && !lexIsToken(WhereToken) && !lexIsToken(EofToken))
            lexNextToken();
    }
    return cond;
}

// Is this type parameter bound to a function signature, '[F fn(u f32) f32]' or
// 'where F is fn(u f32) f32'?
static int parseGenericParmSigBound(GenVarDclNode *parm, INode *cond) {
    if (cond == NULL)
        return 0;
    if (cond->tag == AndLogicTag)
        return parseGenericParmSigBound(parm, ((LogicNode*)cond)->lexp)
            || parseGenericParmSigBound(parm, ((LogicNode*)cond)->rexp);
    if (cond->tag != IsTag)
        return 0;
    CastNode *clause = (CastNode*)cond;
    return clause->typ && clause->typ->tag == FnSigTag && isNameUseNode(clause->exp)
        && ((NameUseNode*)clause->exp)->namesym == parm->namesym;
}

// A generic function joins an overload set only when every one of its type
// parameters is bound to a function signature: a call then picks it by the
// callables it is given (a closure, a function, a struct with a '()'), which a
// call can tell from another overload's parameters. Any other generic would
// need rules to rank it against the rest, and has none, so the declaration is
// refused and does not join.
static void parseGenericOverloadVet(FnDclNode *fnnode) {
    INode **parmp;
    uint32_t cnt;
    for (nodesFor(fnnode->genericinfo->parms, cnt, parmp)) {
        GenVarDclNode *parm = (GenVarDclNode*)*parmp;
        int bound = 0;
        if (parm->annot) {
            INode **annotp;
            uint32_t acnt;
            for (nodesFor(parm->annot, acnt, annotp))
                if ((*annotp)->tag == FnSigTag)
                    bound = 1;
        }
        if (!bound && fnnode->where) {
            INode **condp;
            uint32_t ccnt;
            for (nodesFor(fnnode->where, ccnt, condp))
                if (parseGenericParmSigBound(parm, *condp))
                    bound = 1;
        }
        if (!bound) {
            errorMsgNode((INode*)fnnode, ErrorGenericOverload,
                "A generic function may join the overload set %s only when every type parameter is bound to a function signature, as in `[F fn(u f32) f32]`, and %s is not. An overload is chosen by what its arguments are, and a generic with another kind of parameter has no rule to rank it against the rest.",
                &fnnode->overloadsym->namestr, &parm->namesym->namestr);
            fnnode->overloadsym = NULL;
            return;
        }
    }
}

// Parse a list of generic variables and add to the genericnode.
//
// What follows a parameter's name, before its ',' or ']', is its annotation:
// '[T Comparable]', '[T Comparable + Float]'. The slot is one, for whatever a
// parameter will be annotated with, and it is read as '+'-joined types whose
// meaning is what each resolves to (genericConstraintsNameRes): a trait
// constrains a type parameter, which is built; a type would make a value
// parameter, '[N usize]', and a kind another kind of parameter, '[e Expr]',
// and neither is. 'annotate' is set for a generic function or type; a macro's
// parameter and a generic module's take no annotation yet, and it is refused
// here as it always was.
//
// A struct declares its lifetimes in the same brackets, ''a', apart from its
// type parameters: they go to '*lifes' (lifetime.h), NULL for a declaration
// that declares none, which refuses them.
//
// A type parameter's lifetime bounds, '[T + 'a]', '[T Trait + 'a]', go to
// '*bounds' as ''+T' >= ''a' (lifetime.h, "Lifetime bounds"), for a function
// to add to its signature's order; NULL refuses them (a generic type's).
Nodes *parseGenericParms(ParseState *parse, int annotate, LifeParms **lifes, LifeOrder **bounds) {
    lexNextToken(); // Go past left square bracket
    Nodes *parms = newNodes(2);
    int nlifes = 0;
    while (lexIsToken(IdentToken) || lexIsToken(LifetimeToken)) {
        if (lexIsToken(LifetimeToken)) {
            INode *at = (INode*)newNameUseNode(lex->val.ident);
            if (lifes == NULL)
                errorMsgLex(ErrorLifetimePlace, "Only a struct or an enum declares lifetimes in its brackets: a function names its own in its signature.");
            else {
                if (*lifes == NULL)
                    *lifes = newLifeParms();
                lifeParmsDeclare(*lifes, lex->val.ident, at);
            }
            ++nlifes;
            lexNextToken();
            if (lexIsToken(CommaToken))
                lexNextToken();
            continue;
        }
        GenVarDclNode *parm = newGVarDclNode(lex->val.ident);
        nodesAdd(&parms, (INode*)parm);
        lexNextToken();
        if (lexIsToken(CommaToken))
            lexNextToken();
        // An annotation begins with a name: a trait's, a type's, a kind's;
        // or, for a bound alone, '[T + 'a]', with the '+'
        else if (annotate && (lexIsToken(IdentToken) || lexIsToken(FnToken) || (lexIsToken(PlusToken) && lexPeekIsLifetime()))) {
            // A bound may be a function signature, written bare: 'F fn(a &T, b &T) i32'
            if (lexIsToken(FnToken)) {
                parm->annot = newNodes(2);
                nodesAdd(&parm->annot, parseFnBound(parse));
            }
            else if (lexIsToken(IdentToken)) {
                parm->annot = newNodes(2);
                nodesAdd(&parm->annot, parseType(parse));
            }
            while (lexIsToken(PlusToken)) {
                lexNextToken();
                if (lexIsToken(LifetimeToken)) {
                    parseBoundAdd(bounds, bounds != NULL, parm->namesym);
                    continue;
                }
                if (parm->annot == NULL)
                    parm->annot = newNodes(2);
                nodesAdd(&parm->annot, lexIsToken(FnToken) ? parseFnBound(parse) : parseTypeReq(parse, "'+'"));
            }
            // Every trait named here is required; a choice between them is
            // said in a 'where' clause [Jon 27 Sep]. The annotation is
            // dropped, so nothing it only half says is asked of the arguments.
            if (lexIsToken(OrToken)) {
                errorMsgLex(ErrorGenParmOr, "The traits after a type parameter are joined with '+', each required. A choice between them is written in a 'where' clause: 'where T is A or T is B'.");
                parm->annot = NULL;
                while (!lexIsToken(CommaToken) && !lexIsToken(RBracketToken)) {
                    if (lexIsToken(SemiToken) || lexIsToken(LCurlyToken) || lexIsToken(RCurlyToken) || lexIsToken(EofToken))
                        break;
                    lexNextToken();
                }
            }
            // A parameter ends at its ',' or at the ']'
            if (!lexIsToken(CommaToken))
                break;
            lexNextToken();
        }
        // A second name straight after the first is what a constraint or a
        // parameter type is spelled as -- '[a i32]' on a macro. Neither is
        // implemented there, and reading the two names as two parameters
        // instead turned that into an arity complaint about a declaration
        // written in that form. Refuse it here and resync to the next ',' or ']'.
        else if (lexIsToken(IdentToken) || lexIsToken(FnToken)) {
            errorMsgLex(ErrorGenParmConstr, "A macro's or a generic module's parameter may not carry a constraint or a type: neither is implemented. Separate two parameters with a comma.");
            // (a signature's own commas are inside its parentheses)
            int depth = 0;
            while (depth > 0 || (!lexIsToken(CommaToken) && !lexIsToken(RBracketToken))) {
                if (lexIsToken(SemiToken) || lexIsToken(LCurlyToken) || lexIsToken(RCurlyToken) || lexIsToken(EofToken))
                    break;
                if (lexIsToken(LParenToken))
                    ++depth;
                else if (lexIsToken(RParenToken) && depth > 0)
                    --depth;
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
    if (parms->used == 0 && nlifes == 0)
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
        macro->parms = parseGenericParms(parse, 0, NULL, NULL);
    }
    macro->body = parseExprBlock(parse, 0);
    return macro;
}

// '@compute(x[, y[, z]])' after 'fn': a compute entry point, and the size of
// its workgroup, each a constant integer, those not written 1. WebGPU
// guarantees a workgroup of 256 invocations, and 64 along z, and no more, so
// a larger one is refused here, where the size is written. Returns whether
// the attribute was there; a size refused leaves the function an entry point
// of one invocation, so that its signature is still checked.
static int parseComputeAttr(FnDclNode *fnnode) {
    if (!lexIsToken(ComputeAttrToken))
        return 0;
    lexNextToken();
    uint64_t size[3] = {1, 1, 1};
    int bad = 0;
    if (!lexIsToken(LParenToken)) {
        errorMsgLex(ErrorComputeSize, "'@compute' takes its workgroup's size: '@compute(64)', '@compute(8, 8)' or '@compute(4, 4, 4)'.");
        bad = 1;
    }
    else {
        lexNextToken();
        int n = 0;
        while (1) {
            if (n == 3) {
                errorMsgLex(ErrorComputeSize, "A workgroup has at most three dimensions: '@compute(x, y, z)'.");
                bad = 1;
            }
            else if (!lexIsToken(IntLitToken)) {
                errorMsgLex(ErrorComputeSize, "A workgroup's size is a constant integer, each of up to three written in '@compute(x, y, z)'.");
                bad = 1;
            }
            if (bad) {
                while (!lexIsToken(RParenToken) && !lexIsToken(SemiToken) && !lexIsToken(LCurlyToken) && !lexIsToken(EofToken))
                    lexNextToken();
                break;
            }
            size[n++] = lex->val.uintlit;
            lexNextToken();
            if (!lexIsToken(CommaToken))
                break;
            lexNextToken();
        }
        parseCloseTok(RParenToken);
    }
    if (!bad) {
        if (size[0] == 0 || size[1] == 0 || size[2] == 0) {
            errorMsgNode((INode*)fnnode, ErrorComputeSize, "A workgroup's size is at least 1 each way.");
            bad = 1;
        }
        else if (size[2] > 64) {
            errorMsgNode((INode*)fnnode, ErrorComputeSize,
                "A workgroup may be at most 64 invocations along z, WebGPU's guaranteed limit; this one is %u.", (unsigned)size[2]);
            bad = 1;
        }
        else if (size[0] > 256 || size[1] > 256 || size[0] * size[1] * size[2] > 256) {
            errorMsgNode((INode*)fnnode, ErrorComputeSize,
                "A workgroup may hold at most 256 invocations, WebGPU's guaranteed limit; %llu by %llu by %llu is %llu.",
                (unsigned long long)size[0], (unsigned long long)size[1], (unsigned long long)size[2],
                (unsigned long long)(size[0] * size[1] * size[2]));
            bad = 1;
        }
    }
    for (int i = 0; i < 3; ++i)
        fnnode->compute[i] = bad ? 1 : (uint16_t)size[i];
    return 1;
}

// Parse a function block
INode *parseFn(ParseState *parse, uint16_t mayflags) {
    FnDclNode *fnnode = newFnDclNode(NULL, 0, NULL, NULL);
    LifeOrder *bounds = NULL;   // its type parameters' lifetime bounds, '[T + 'a]'
    char *fntparms = NULL, *fntparmsend = NULL;     // where its type parameters are written

    // Skip past the 'fn'.
    lexNextToken();

    // '@c' after the keyword: this function's symbol is a C name, its own name
    // or the string written, and 'system' its calling convention [Jon 23 Sep].
    // '@initpure' there too, before or after it: a function a module's 'init'
    // may call, as 'init' itself is declared (refmodule.html, "Dynamic
    // initialization"). Recorded, not yet checked
    // '@intrinsic' there too, first or after the others: a function whose meaning
    // the compiler supplies, declared in core (refintrinsic.html). Where it may
    // be and what it must say is checked by intrinsicDclNameRes
    // '@compute(x, y, z)' there too, first or after the others: a compute
    // entry point (fnDclComputeCheck checks its signature)
    int compute = parseComputeAttr(fnnode);
    int intrinsic = 0;
    if (lexIsToken(IntrinsicAttrToken)) {
        intrinsic = 1;
        lexNextToken();
    }
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
    if (!intrinsic && lexIsToken(IntrinsicAttrToken)) {
        intrinsic = 1;
        lexNextToken();
    }
    // A function has no storage to give each thread or workgroup a copy of
    parseStorageAttr(0);
    if (!compute)
        compute = parseComputeAttr(fnnode);
    if (initpure)
        fnnode->dclinfo.facts |= DclInitPure;
    // An intrinsic's meaning is the compiler's, so its body is optional: the
    // fallback a back end with no lowering of its own uses
    if (intrinsic) {
        fnnode->dclinfo.facts |= DclIntrinsic;
        mayflags |= ParseMaySig;
    }

    // Process function name, if provided. The lexer reads a permission's name
    // ('mut', 'opaq', ...) as the permission wherever it is written, so it
    // cannot name a function. It is reported once and read past, so the rest of
    // the declaration parses as written instead of cascading from a missing
    // name. The function is left unnamed, which keeps it out of every namespace:
    // bound, it would also clash with the built-in permission
    if (lexIsToken(IdentToken) || lexIsToken(PermToken)) {
        if (lexIsToken(PermToken))
            errorMsgLex(ErrorNoName, "'%s' is a permission, so it cannot name a function. Rename it.",
                &lex->val.ident->namestr);
        else {
            if (!(mayflags&ParseMayName))
                errorMsgLex(WarnName, "Unnecessary function name is ignored");
            fnnode->namesym = lex->val.ident;
        }
        lexNextToken();
        if (lexIsToken(LBracketToken)) {
            // Where its type parameters are written, which a generator copies (parsegen.c)
            char *tparms = lex->tokp + 1;
            fnnode->genericinfo = newGenericInfo();
            fnnode->genericinfo->parms = parseGenericParms(parse, 1, NULL, &bounds);
            fntparms = tparms;
            fntparmsend = lex->prevend - 1;
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
            else
                // A generic's right to name a set is judged once its bounds are
                // all read (parseGenericOverloadVet)
                fnnode->overloadsym = overloadsym;
        }
    }

    // Process the function's signature info.
    // After '&fn' (ParseEmbedded), the signature is a function-reference type
    // unless a body follows, and its parameters are settled once that is known
    int reftype = (mayflags & ParseEmbedded) != 0;
    int errorsAtSig = errors;
    fnnode->vtype = parseFnSig(parse, reftype);
    // 'yields': a generator (parsegen.c). Its signature, read from the text
    // the parameters and the yielded type are written in
    int isgen = !reftype && parse->isgen;
    GenSig gensig = parse->gensig;
    gensig.tparms = fntparms;
    gensig.tparmsend = fntparmsend;
    gensig.tnames = fnnode->genericinfo ? fnnode->genericinfo->parms : NULL;

    // Its type parameters' lifetime bounds join the order among its
    // signature's lifetimes, as a 'where' clause's do
    if (bounds && fnnode->vtype->tag == FnSigTag) {
        LifeOrder **orderp = &((FnSigNode*)fnnode->vtype)->lifeorder;
        if (*orderp == NULL)
            *orderp = newLifeOrder();
        for (uint32_t i = 0; i < bounds->count; ++i)
            lifeOrderAdd(*orderp, bounds->pairs[2 * i], bounds->pairs[2 * i + 1], bounds->at[i]);
    }

    // Handle optional specification that we are declaring an inline function,
    // one whose implementation will be "inlined" into any function that calls it
    if (lexIsToken(InlineToken)) {
        fnnode->flags |= FlagInline;
        lexNextToken();
    }

    // Its constraints, just before the block: a generic function's
    // requirements, or a generic type's method's conditions for existing; and
    // any function's order among its signature's lifetimes
    if (lexIsToken(WhereToken))
        parseWhere(parse, &fnnode->where, &((FnSigNode*)fnnode->vtype)->lifeorder, 1);

    if (fnnode->genericinfo && fnnode->overloadsym)
        parseGenericOverloadVet(fnnode);

    // '@c' names a symbol, so it goes only where a function has one of its own
    if (hasc) {
        INsTypeNode *type = parse->typenode;
        const char *why = NULL;
        if (fnnode->namesym == NULL)
            why = "An anonymous function has no name for '@c' to give C spelling to.";
        else if (intrinsic)
            why = "An intrinsic is expanded where it is called and leaves no symbol for '@c' to name.";
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

    if (reftype)
        parseFnSigSettle(parse, (FnSigNode*)fnnode->vtype, !parseHasBlock());

    // Process statements block that implements function, if provided
    char *bodyp = NULL, *bodyendp = NULL;
    if (parseHasBlock()) {
        if (!(mayflags&ParseMayImpl))
            errorMsgNode((INode*)fnnode, ErrorBadImpl, "Function/method implementation is not allowed here.");
        bodyp = lex->tokp;
        uint32_t awaits = parse->dcltexts ? parse->dcltexts->awaits : 0;
        // A generator's body reads 'yield' and its 'return'; a function nested
        // in it, which is no generator, reads neither
        if (isgen) {
            const char *why = NULL;
            if (fnnode->namesym == NULL)
                why = "An anonymous function cannot be a generator: a generator is a function with a name, which makes a value that is walked.";
            else if (fntparms && memchr(fntparms, '\'', fntparmsend - fntparms))
                why = "A generator's type parameters cannot name lifetimes yet: the generator is a struct of its own that copies them.";
            else if (parse->typenode && (((INode*)parse->typenode)->flags & (TraitType | EnumType)))
                why = "A trait's or an enum's method cannot be a generator yet: the generator is a struct of its own that holds the method's parameters, 'self' among them, and a trait's method is copied into each implementer. Write a function that takes what the method would.";
            else if (parse->dcltexts)
                why = "An actor's method cannot be a generator yet: the actor's body is read to generate its mailbox and its state, and a generator's struct is not one of them. Write a function that takes what the method would.";
            else if (parse->typenode && ((INode*)parse->typenode)->tag == StructTag && ((StructNode*)parse->typenode)->lifeparms)
                why = "A method of a type that declares lifetimes cannot be a generator yet: the generator is a struct of its own that holds the receiver, and does not declare the type's lifetimes.";
            else if (fnnode->flags & FlagInline)
                why = "A generator cannot be 'inline': it is a struct and a method, not code expanded where it is called.";
            else if (fnnode->where)
                why = "A generator cannot have a 'where' clause yet.";
            else if (hasc || compute || intrinsic)
                why = "A generator is a struct and a method, so it has no symbol for '@c' to name, cannot be an '@intrinsic', and is no '@compute' entry point.";
            // Refused, but its body is still read as a generator's, so that
            // its 'yield' is not reported as well
            if (why)
                errorMsgNode((INode*)fnnode, ErrorGenForm, "%s", why);
        }
        GenCtx *genctx = isgen ? parseGenBegin(parse, fnnode) : NULL;
        GenCtx *svgenctx = parse->genctx;
        int svgenoperand = parse->genoperand;
        parse->genctx = genctx;
        parse->genoperand = 0;
        fnnode->value = parseExprBlock(parse, 0);
        parse->genctx = svgenctx;
        parse->genoperand = svgenoperand;
        bodyendp = lex->prevend;
        if (genctx && errors == errorsAtSig) {
            // The declarations the generator stands for are read from text of
            // their own, which leaves where they are written in the parse state;
            // the span the caller records is the author's declaration
            FnDclNode *ctor = parseGenFinish(parse, fnnode, &gensig, genctx, (BlockNode*)fnnode->value);
            parse->bodyp = bodyp;
            parse->bodyendp = bodyendp;
            return (INode*)ctor;
        }
        // In an actor's body, a method holding an 'await' is noted: its
        // dispatch and its actor's state are generated for its seams
        // (parseactor.c)
        if (parse->dcltexts && parse->dcltexts->awaits != awaits)
            nodesAdd(&parse->dcltexts->awaiting, (INode*)fnnode);
    }
    else {
        if (isgen)
            errorMsgNode((INode*)fnnode, ErrorGenForm,
                "A generator is its body: 'yields' declares a function that hands its caller values, so a declaration with no body, an 'extern' one among them, is not built.");
        else if (!(mayflags&ParseMaySig))
            errorMsgNode((INode*)fnnode, ErrorNoImpl, "Function/method must be implemented.");
        if (!(mayflags&ParseEmbedded))
            parseEndOfStatement();
    }

    // An entry point is a function of a module's own, called by nothing but a
    // dispatch: the GPU runs its body, and the CPU calls it by its name
    if (compute) {
        INsTypeNode *type = parse->typenode;
        const char *why = NULL;
        if (fnnode->namesym == NULL)
            why = "An anonymous function has no name for a dispatch to run it by, so it cannot be '@compute'.";
        else if (type)
            why = "A method or a type's function cannot be '@compute': an entry point is a function of its module, run by its name.";
        else if (intrinsic)
            why = "An intrinsic's meaning is the compiler's, so it cannot be '@compute'.";
        else if (fnnode->genericinfo)
            why = "A generic function is no one function until instanced, so it cannot be '@compute': an entry point's types are fixed.";
        else if (fnnode->flags & FlagInline)
            why = "An inline function is expanded where it is called and leaves nothing for a dispatch to run, so it cannot be '@compute'.";
        else if (hasc)
            why = "'@c' names a function for C to call, and a compute entry point is run by a dispatch, so the two do not go together.";
        // An 'extern' one, as a package's include file declares it, is
        // defined in the package, and called on the CPU as any function is;
        // one with no body where one must be written is refused as such
        else if (fnnode->value == NULL && (mayflags & ParseMayImpl) && (mayflags & ParseMaySig))
            why = "An entry point is defined where it is declared: '@compute' needs the function's body.";
        if (why) {
            errorMsgNode((INode*)fnnode, ErrorComputeAttr, "%s", why);
            fnnode->compute[0] = fnnode->compute[1] = fnnode->compute[2] = 0;
        }
    }

    // Where the body is, for the span the caller records
    parse->bodyp = bodyp;
    parse->bodyendp = bodyendp;
    return (INode*)fnnode;
}
