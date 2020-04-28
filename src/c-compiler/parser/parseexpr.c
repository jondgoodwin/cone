/** Parse expressions
 * @file
 *
 * The parser translates the lexer's tokens into IR nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ir/ir.h"
#include "../ir/nametbl.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "lexer.h"

#include <stdio.h>
#include <assert.h>

INode *parseAddr(ParseState *parse);
INode *parseList(ParseState *parse, INode *typenode);

// Parse a name use, which may be qualified with module names
INode *parseNameUse(ParseState *parse) {
    NameUseNode *nameuse = newNameUseNode(NULL);

    int baseset = 0;
    if (lexIsToken(DblColonToken)) {
        nameUseBaseMod(nameuse, parse->pgmmod);
        baseset = 1;
    }
    while (1) {
        if (lexIsToken(IdentToken)) {
            Name *name = lex->val.ident;
            lexNextToken();
            // Identifier is a module qualifier
            if (lexIsToken(DblColonToken)) {
                if (!baseset)
                    nameUseBaseMod(nameuse, parse->mod); // relative to current module
                nameUseAddQual(nameuse, name);
                lexNextToken();
            }
            // Identifier is the actual name itself
            else {
                nameuse->namesym = name;
                break;
            }
        }
        // Can only get here if previous token was double quotes
        else {
            errorMsgLex(ErrorNoVar, "Missing variable name after module qualifiers");
            break;
        }
    }
    return (INode*)nameuse;
}

// Parse a term: literal, identifier, etc.
INode *parseTerm(ParseState *parse) {
    switch (lex->toktype) {
    case trueToken:
    {
        ULitNode *node = newULitNode(1, (INode*)boolType);
        lexNextToken();
        return (INode *)node;
    }
    case falseToken:
    {
        ULitNode *node = newULitNode(0, (INode*)boolType);
        lexNextToken();
        return (INode *)node;
    }
    case nullToken:
    {
        NullNode *node = newNullNode();
        lexNextToken();
        return (INode *)node;
    }
    case IntLitToken:
        {
            ULitNode *node = newULitNode(lex->val.uintlit, lex->langtype);
            lexNextToken();
            return (INode *)node;
        }
    case FloatLitToken:
        {
            FLitNode *node = newFLitNode(lex->val.floatlit, lex->langtype);
            lexNextToken();
            return (INode *)node;
        }
    case StringLitToken:
        {
            SLitNode *node = newSLitNode(lex->val.strlit, lex->langtype);
            lexNextToken();
            return (INode *)node;
        }
    case IdentToken:
    case DblColonToken:
        return (INode*)parseNameUse(parse);
    case LParenToken:
        {
            INode *node;
            lexNextToken();
            lexIncrParens();
            node = parseAnyExpr(parse);
            parseCloseTok(RParenToken);
            return node;
        }
    case LBracketToken:
        return parseList(parse, NULL);
    default:
        errorMsgLex(ErrorBadTerm, "Invalid term: expected name, literal, etc.");
        lexNextToken(); // Avoid infinite loop
        return NULL;
    }
}

// Parse a function/method call argument
INode *parseArg(ParseState *parse) {
    INode *arg = parseSimpleExpr(parse);
    if (lexIsToken(ColonToken)) {
        if (arg->tag != NameUseTag)
            errorMsgNode((INode*)arg, ErrorNoName, "Expected a named identifier");
        arg = (INode*)newNamedValNode(arg);
        lexNextToken();
        ((NamedValNode *)arg)->val = parseSimpleExpr(parse);
    }
    return arg;
}

INode *parsePostCall(ParseState *parse, INode *node, uint16_t flags);

// Construct a FnCall node, by parsing its suffices on a passed operand node
// We enter here knowing that next token is a '.', '(' or '['
INode *parseFnCall(ParseState *parse, INode *node, uint16_t flags) {
    FnCallNode *fncall = newFnCallNode(node, 0);
    fncall->flags |= flags;

    if (lexIsToken(DotToken)) {
        lexNextToken();

        // Get field/method name
        if (!lexIsToken(IdentToken)) {
            errorMsgLex(ErrorNoMbr, "This should be a named field/method");
            lexNextToken();
            return (INode*)fncall;
        }
        NameUseNode *method = newNameUseNode(lex->val.ident);
        method->tag = MbrNameUseTag;
        fncall->methfld = method;
        lexNextToken();

        // Square brackets after '.' are a separate fncall operation
        if (lexIsToken(LBracketToken))
            return parsePostCall(parse, (INode*)fncall, flags);

    }

    // Handle () or [] suffixes and their enclosed arguments
    if ((lexIsToken(LParenToken) || lexIsToken(LBracketToken)) && !lexIsStmtBreak()) {
        int closetok;
        if (lex->toktype == LBracketToken) {
            fncall->flags |= FlagIndex;
            closetok = RBracketToken;
        }
        else
            closetok = RParenToken;
        lexNextToken();
        lexIncrParens();
        fncall->args = newNodes(8);
        if (!lexIsToken(closetok)) {
            nodesAdd(&fncall->args, parseArg(parse));
            while (lexIsToken(CommaToken)) {
                lexNextToken();
                nodesAdd(&fncall->args, parseArg(parse));
            }
        }
        parseCloseTok(closetok);
    }

    // Recursively wrap additional call suffixes, if any. Return fncall node otherwise.
    if (lexIsToken(DotToken) || lexIsToken(LParenToken) || lexIsToken(LBracketToken))
        return parsePostCall(parse, (INode*)fncall, flags);
    return (INode*)fncall;
}

// Handle call suffix operators '.', '()', '[]' as postfix/infix operators
INode *parsePostCall(ParseState *parse, INode *node, uint16_t flags) {
    if ((lexIsToken(DotToken) || lexIsToken(LParenToken) || lexIsToken(LBracketToken)) && !lexIsStmtBreak())
        return parseFnCall(parse, node, flags);
    return node;
}

// Parse prefix dot operator
INode *parseDotPrefix(ParseState *parse) {
    // '.' as prefix operator implies 'this' as operand
    if (lexIsToken(DotToken))
        return parseFnCall(parse, (INode*)newNameUseNode(thisName), 0);
    else
        return parsePostCall(parse, parseTerm(parse), 0);
}

// Parse an "address term" for:
// - ref to an anonymous function
// - Allocation
// - Borrowed ref
INode *parseAddr(ParseState *parse) {
    // Parse when it is not address operator
    if (!lexIsToken(AmperToken))
        return parseDotPrefix(parse);

    // It is address operator ...
    RefNode *reftype = newRefNode();  // Type for address node
    reftype->pvtype = unknownType;    // Type inference will correct this
    reftype->region = borrowRef;

    lexNextToken();

    // Borrowed reference to anonymous function/closure
    if (lexIsToken(FnToken)) {
        BorrowNode *anode = newBorrowNode();
        anode->vtype = (INode *)reftype;
        INode *fndcl = parseFn(parse, 0, ParseMayAnon | ParseMayImpl);
        nodesAdd(&parse->mod->nodes, fndcl);
        NameUseNode *fnname = newNameUseNode(anonName);
        fnname->tag = VarNameUseTag;
        fnname->dclnode = fndcl;
        fnname->vtype = ((FnDclNode *)fndcl)->vtype;
        anode->exp = (INode*)fnname;
        reftype->perm = (INode*)opaqPerm;
        return (INode *)anode;
    }

    // Allocated reference
    if (lexIsToken(IdentToken)
        && lex->val.ident->node && lex->val.ident->node->tag == RegionTag) {
        reftype->region = (INode*)lex->val.ident->node;
        AllocateNode *anode = newAllocateNode();
        anode->vtype = (INode *)reftype;
        lexNextToken();
        reftype->perm = parsePerm(uniPerm);
        anode->exp = parseDotPrefix(parse);
        return (INode *)anode;
    }

    // Borrowed reference
    BorrowNode *anode = newBorrowNode();
    reftype->perm = parsePerm(NULL);
    anode->vtype = (INode *)reftype;

    // Get the term we are borrowing from (which might be a de-referenced reference)
    if (lexIsToken(StarToken)) {
        DerefNode *derefnode = newDerefNode();
        lexNextToken();
        derefnode->exp = lexIsToken(DotToken) ? (INode*)newNameUseNode(thisName) : parseTerm(parse);
        anode->exp = (INode*)derefnode;
    }
    else
        anode->exp = lexIsToken(DotToken) ? (INode*)newNameUseNode(thisName) : parseTerm(parse);

    // Process borrow chain suffixes, and set flag to show if we had some
    INode* node = parsePostCall(parse, (INode*)anode, FlagBorrow);
    if (node != (INode*)anode)
        anode->flags |= FlagSuffix;
    return node;
}

// Parse postfix ++ and -- operators
INode *parsePostIncr(ParseState *parse) {
    INode *node = parseAddr(parse);
    while (1) {
        if (lexIsToken(IncrToken) && !lexIsStmtBreak()) {
            node = (INode*)newFnCallOpname(node, incrPostName, 0);
            node->flags |= FlagLvalOp;
            lexNextToken();
        }
        else if (lexIsToken(DecrToken) && !lexIsStmtBreak()) {
            node = (INode*)newFnCallOpname(node, decrPostName, 0);
            node->flags |= FlagLvalOp;
            lexNextToken();
        }
        else {
            return node;
        }
    }
}

// Parse a prefix operator: - (negative), ~ (not), ++, --, * (deref)
INode *parsePrefix(ParseState *parse) {
    switch (lex->toktype) {
    case DashToken:
    {
        FnCallNode *node = newFnCallOpname(NULL, minusName, 0);
        lexNextToken();
        INode *argnode = parsePrefix(parse);
        if (argnode->tag == ULitTag) {
            ((ULitNode*)argnode)->uintlit = (uint64_t)-((int64_t)((ULitNode*)argnode)->uintlit);
            return argnode;
        }
        else if (argnode->tag == FLitTag) {
            ((FLitNode*)argnode)->floatlit = -((FLitNode*)argnode)->floatlit;
            return argnode;
        }
        node->objfn = argnode;
        return (INode *)node;
    }
    case TildeToken:
    {
        FnCallNode *node = newFnCallOp(NULL, "~", 0);
        lexNextToken();
        node->objfn = parsePrefix(parse);
        return (INode *)node;
    }
    case IncrToken:
    {
        FnCallNode *node = newFnCallOpname(NULL, incrName, 0);
        node->flags |= FlagLvalOp;
        lexNextToken();
        node->objfn = parsePrefix(parse);
        return (INode *)node;
    }
    case DecrToken:
    {
        FnCallNode *node = newFnCallOpname(NULL, decrName, 0);
        node->flags |= FlagLvalOp;
        lexNextToken();
        node->objfn = parsePrefix(parse);
        return (INode *)node;
    }
    case StarToken:
    {
        DerefNode *node = newDerefNode();
        lexNextToken();
        node->exp = parsePrefix(parse);
        return (INode *)node;
    }
    default:
        return parsePostIncr(parse);
    }
}

// Parse type cast
INode *parseCast(ParseState *parse) {
    INode *lhnode = parsePrefix(parse);
    if (lexIsToken(AsToken)) {
        CastNode *node = newRecastNode(lhnode, unknownType);
        lexNextToken();
        node->typ = parseVtype(parse);
        return (INode*)node;
    }
    else if (lexIsToken(IntoToken)) {
        CastNode *node = newConvCastNode(lhnode, unknownType);
        lexNextToken();
        node->typ = parseVtype(parse);
        return (INode*)node;
    }
    return lhnode;
}

// Parse binary multiply, divide, rem operator
INode *parseMult(ParseState *parse) {
    INode *lhnode = parseCast(parse);
    while (1) {
        if (lexIsToken(StarToken) && !lexIsStmtBreak()) {
            FnCallNode *node = newFnCallOpname(lhnode, multName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseCast(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(SlashToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, divName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseCast(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(PercentToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, remName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseCast(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse binary add, subtract operator
INode *parseAdd(ParseState *parse) {
    INode *lhnode = parseMult(parse);
    while (1) {
        if (lexIsToken(PlusToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, plusName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseMult(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(DashToken) && !lexIsStmtBreak()) {
            FnCallNode *node = newFnCallOpname(lhnode, minusName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseMult(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse << and >> operators
INode *parseShift(ParseState *parse) {
    INode *lhnode;
    // Prefix '<<' or '>>' implies 'this'
    if (lexIsToken(ShlToken) || lexIsToken(ShrToken))
        lhnode = (INode *)newNameUseNode(thisName);
    else
        lhnode = parseAdd(parse);
    while (1) {
        if (lexIsToken(ShlToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, shlName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseAdd(parse));
            lhnode = (INode*)node;
        }
        else if (lexIsToken(ShrToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, shrName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseAdd(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse bitwise And
INode *parseAnd(ParseState *parse) {
    INode *lhnode = parseShift(parse);
    while (1) {
        if (lexIsToken(AmperToken) && !lexIsStmtBreak()) {
            FnCallNode *node = newFnCallOpname(lhnode, andName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseShift(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse bitwise Xor
INode *parseXor(ParseState *parse) {
    INode *lhnode = parseAnd(parse);
    while (1) {
        if (lexIsToken(CaretToken)) {
            FnCallNode *node = newFnCallOpname(lhnode, xorName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseAnd(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse bitwise or
INode *parseOr(ParseState *parse) {
    INode *lhnode = parseXor(parse);
    while (1) {
        if (lexIsToken(BarToken) && !lexIsStmtBreak()) {
            FnCallNode *node = newFnCallOpname(lhnode, orName, 2);
            lexNextToken();
            nodesAdd(&node->args, parseXor(parse));
            lhnode = (INode*)node;
        }
        else
            return lhnode;
    }
}

// Parse comparison operator
INode *parseCmp(ParseState *parse) {
    INode *lhnode = parseOr(parse);
    char *cmpop;

    switch (lex->toktype) {
    case EqToken:  cmpop = "=="; break;
    case NeToken:  cmpop = "!="; break;
    case LtToken:  cmpop = "<"; break;
    case LeToken:  cmpop = "<="; break;
    case GtToken:  cmpop = ">"; break;
    case GeToken:  cmpop = ">="; break;
    default:
        if (lexIsToken(IsToken) && !lexIsStmtBreak()) {
            CastNode *node = newIsNode(lhnode, unknownType);
            lexNextToken();
            node->typ = parseVtype(parse);
            return (INode*)node;
        }
        else
            return lhnode;
    }

    FnCallNode *node = newFnCallOp(lhnode, cmpop, 2);
    lexNextToken();
    nodesAdd(&node->args, parseOr(parse));
    return (INode*)node;
}

// Parse 'not' logical operator
INode *parseNotLogic(ParseState *parse) {
    if (lexIsToken(NotToken)) {
        LogicNode *node = newLogicNode(NotLogicTag);
        lexNextToken();
        node->lexp = parseNotLogic(parse);
        return (INode*)node;
    }
    return parseCmp(parse);
}

// Parse 'and' logical operator
INode *parseAndLogic(ParseState *parse) {
    INode *lhnode = parseNotLogic(parse);
    while (lexIsToken(AndToken)) {
        LogicNode *node = newLogicNode(AndLogicTag);
        lexNextToken();
        node->lexp = lhnode;
        node->rexp = parseNotLogic(parse);
        lhnode = (INode*)node;
    }
    return lhnode;
}

// Parse 'or' logical operator
INode *parseOrExpr(ParseState *parse) {
    INode *lhnode = parseAndLogic(parse);
    while (lexIsToken(OrToken)) {
        LogicNode *node = newLogicNode(OrLogicTag);
        lexNextToken();
        node->lexp = lhnode;
        node->rexp = parseAndLogic(parse);
        lhnode = (INode*)node;
    }
    return lhnode;
}

// This parses any kind of expression, including blocks, assignment or tuple
INode *parseSimpleExpr(ParseState *parse) {
    switch (lex->toktype) {
    case IfToken:
        return parseIf(parse);
    case MatchToken:
        return parseMatch(parse);
    case LoopToken:
        return parseLoop(parse, NULL);
    case LifetimeToken:
        return parseLifetime(parse, 0);
    case LCurlyToken:
        return parseExprBlock(parse);
    default:
        return parseOrExpr(parse);
    }
}

// Parse a comma-separated expression tuple
INode *parseTuple(ParseState *parse) {
    INode *exp = parseSimpleExpr(parse);
    if (lexIsToken(CommaToken)) {
        TupleNode *tuple = newVTupleNode();
        nodesAdd(&tuple->elems, exp);
        while (lexIsToken(CommaToken)) {
            lexNextToken();
            nodesAdd(&tuple->elems, parseSimpleExpr(parse));
        }
        return (INode*)tuple;
    }
    else
        return exp;
}

// Parse an operator assignment
INode *parseOpEq(ParseState *parse, INode *lval, Name *opeqname) {
    FnCallNode *node = newFnCallOpname(lval, opeqname, 2);
    node->flags |= FlagOpAssgn | FlagLvalOp;
    lexNextToken();
    nodesAdd(&node->args, parseAnyExpr(parse));
    return (INode*)node;
}

// Parse an assignment expression
INode *parseAssign(ParseState *parse) {
    INode *lval = parseTuple(parse);
    switch (lex->toktype) {
    case AssgnToken:
    {
        lexNextToken();
        INode *rval = parseAnyExpr(parse);
        return (INode*)newAssignNode(NormalAssign, lval, rval);
    }

    case PlusEqToken:
        return parseOpEq(parse, lval, plusEqName);
    case MinusEqToken:
        return parseOpEq(parse, lval, minusEqName);
    case MultEqToken:
        return parseOpEq(parse, lval, multEqName);
    case DivEqToken:
        return parseOpEq(parse, lval, divEqName);
    case RemEqToken:
        return parseOpEq(parse, lval, remEqName);
    case OrEqToken:
        return parseOpEq(parse, lval, orEqName);
    case AndEqToken:
        return parseOpEq(parse, lval, andEqName);
    case XorEqToken:
        return parseOpEq(parse, lval, xorEqName);
    case ShlEqToken:
        return parseOpEq(parse, lval, shlEqName);
    case ShrEqToken:
        return parseOpEq(parse, lval, shrEqName);
    default:
        return lval;
    }
}

INode *parseList(ParseState *parse, INode *typenode) {
    FnCallNode *list = newFnCallNode(NULL, 4);
    list->tag = TypeLitTag;
    ArrayNode *arrtype = newArrayNode();
    list->objfn = typenode;
    list->vtype = (INode*)arrtype;
    lexNextToken();
    lexIncrParens();
    if (!lexIsToken(RBracketToken)) {
        while (1) {
            nodesAdd(&list->args, parseSimpleExpr(parse));
            if (!lexIsToken(CommaToken))
                break;
            lexNextToken();
        };
    }
    arrtype->size = list->args->used;
    parseCloseTok(RBracketToken);
    return (INode*)list;
}

// This parses any kind of expression, including blocks, assignment or tuple
INode *parseAnyExpr(ParseState *parse) {
    return parseAssign(parse);
}
