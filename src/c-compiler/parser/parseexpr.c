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
	case StrLitToken:
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
			node = parseAnyExpr(parse);
			parseCloseTok(RParenToken);
			return node;
		}
    case LBracketToken:
        return parseList(parse, NULL);
	default:
		errorMsgLex(ErrorBadTerm, "Invalid term value: expected variable, literal, etc.");
		return NULL;
	}
}

// Parse the postfix operators: '.', '::', '()', '[]'
INode *parsePostfix(ParseState *parse) {
    INode *node = lexIsToken(DotToken)? (INode*)newNameUseNode(thisName) : parseTerm(parse);

	while (1) {
		switch (lex->toktype) {

		// Function call with possible parameters
		case LParenToken:
        case LBracketToken:
		{
            int closetok = lex->toktype == LBracketToken? RBracketToken : RParenToken;
			FnCallNode *fncall = newFnCallNode(node, 8);
            if (closetok == RBracketToken)
                fncall->flags |= FlagIndex;
			lexNextToken();
			if (!lexIsToken(closetok))
				while (1) {
					nodesAdd(&fncall->args, parseSimpleExpr(parse));
					if (lexIsToken(CommaToken))
						lexNextToken();
					else
						break;
				}
			parseCloseTok(closetok);
			node = (INode *)fncall;
			break;
		}

		// Object call with possible parameters
		case DotToken:
		{
            FnCallNode *fncall = newFnCallNode(node, 0);
            lexNextToken();

			// Get property/method name
			if (!lexIsToken(IdentToken)) {
				errorMsgLex(ErrorNoMbr, "This should be a named property/method");
				lexNextToken();
				break;
			}
			NameUseNode *method = newNameUseNode(lex->val.ident);
			method->tag = MbrNameUseTag;
            fncall->methprop = method;
            lexNextToken();

			// If parameters provided, capture them as part of method call
			if (lexIsToken(LParenToken)) {
				lexNextToken();
                fncall->args = newNodes(8);
				if (!lexIsToken(RParenToken)) {
					while (1) {
						nodesAdd(&fncall->args, parseSimpleExpr(parse));
						if (lexIsToken(CommaToken))
							lexNextToken();
						else
							break;
					}
				}
				parseCloseTok(RParenToken);
			}
            node = (INode *)fncall;
            break;
		}

		default:
			return node;
		}
	}
}

// Parse an address term - current token is '&'
INode *parseAddr(ParseState *parse) {
	AddrNode *anode = newAddrNode();
	lexNextToken();

	// Address node's value type is a partially populated reference type
	RefNode *reftype = newRefNode();
	reftype->pvtype = NULL;     // Type inference will correct this
    if (lexIsToken(IdentToken)
        && lex->val.ident->node && lex->val.ident->node->tag == AllocTag) {
        reftype->alloc = (INode*)lex->val.ident->node;
        lexNextToken();
        reftype->perm = parsePerm(uniPerm);
    }
    else {
        reftype->alloc = voidType;
        reftype->perm = parsePerm(NULL);
    }
	anode->vtype = (INode *)reftype;

	// A value or constructor
	anode->exp = parseTerm(parse);

	return (INode *)anode;
}

// Parse a prefix operator, e.g.: -
INode *parsePrefix(ParseState *parse) {
	switch (lex->toktype) {
	case DashToken:
	{
		FnCallNode *node = newFnCallOp(NULL, "-", 0);
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
	case StarToken:
	{
		DerefNode *node = newDerefNode();
		lexNextToken();
		node->exp = parsePrefix(parse);
		return (INode *)node;
	}
	case AmperToken:
		return parseAddr(parse);
	default:
		return parsePostfix(parse);
	}
}

// Parse type cast
INode *parseCast(ParseState *parse) {
	INode *lhnode = parsePrefix(parse);
	if (lexIsToken(AsToken)) {
		CastNode *node = newCastNode(lhnode, NULL);
		lexNextToken();
		node->vtype = parseVtype(parse);
		return (INode*)node;
	}
	return lhnode;
}

// Parse binary multiply, divide, rem operator
INode *parseMult(ParseState *parse) {
	INode *lhnode = parseCast(parse);
	while (1) {
		if (lexIsToken(StarToken)) {
			FnCallNode *node = newFnCallOp(lhnode, "*", 2);
			lexNextToken();
			nodesAdd(&node->args, parseCast(parse));
			lhnode = (INode*)node;
		}
		else if (lexIsToken(SlashToken)) {
			FnCallNode *node = newFnCallOp(lhnode, "/", 2);
			lexNextToken();
			nodesAdd(&node->args, parseCast(parse));
			lhnode = (INode*)node;
		}
		else if (lexIsToken(PercentToken)) {
			FnCallNode *node = newFnCallOp(lhnode, "%", 2);
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
			FnCallNode *node = newFnCallOp(lhnode, "+", 2);
			lexNextToken();
			nodesAdd(&node->args, parseMult(parse));
			lhnode = (INode*)node;
		}
		else if (lexIsToken(DashToken)) {
			FnCallNode *node = newFnCallOp(lhnode, "-", 2);
			lexNextToken();
			nodesAdd(&node->args, parseMult(parse));
			lhnode = (INode*)node;
		}
		else
			return lhnode;
	}
}

// Parse bitwise And
INode *parseAnd(ParseState *parse) {
	INode *lhnode = parseAdd(parse);
	while (1) {
		if (lexIsToken(AmperToken)) {
			FnCallNode *node = newFnCallOp(lhnode, "&", 2);
			lexNextToken();
			nodesAdd(&node->args, parseAdd(parse));
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
			FnCallNode *node = newFnCallOp(lhnode, "^", 2);
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
		if (lexIsToken(BarToken)) {
			FnCallNode *node = newFnCallOp(lhnode, "|", 2);
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
	default: return lhnode;
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
INode *parseSimpleExpr(ParseState *parse) {
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

// Parse a comma-separated expression tuple
INode *parseTuple(ParseState *parse) {
    INode *exp = parseSimpleExpr(parse);
    if (lexIsToken(CommaToken)) {
        VTupleNode *tuple = newVTupleNode();
        nodesAdd(&tuple->values, exp);
        while (lexIsToken(CommaToken)) {
            lexNextToken();
            nodesAdd(&tuple->values, parseSimpleExpr(parse));
        }
        return (INode*)tuple;
    }
    else
        return exp;
}

// Parse an assignment expression
INode *parseAssign(ParseState *parse) {
    // Prefix '<<' implies 'this' as lval
    if (lexIsToken(LtltToken)) {
        NameUseNode *thisvar = newNameUseNode(thisName);
        FnCallNode *node = newFnCallOp((INode*)thisvar, "<<", 2);
        lexNextToken();
        nodesAdd(&node->args, parseAnyExpr(parse));
        return (INode*)node;
    }

    INode *lval = parseTuple(parse);
	if (lexIsToken(AssgnToken)) {
		lexNextToken();
		INode *rval = parseAnyExpr(parse);
		return (INode*) newAssignNode(NormalAssign, lval, rval);
	}
    // sym:rval => this.sym = rval
    else if (lexIsToken(ColonToken)) {
        lexNextToken();
        INode *rval = parseAnyExpr(parse);
        NameUseNode *thisnode = newNameUseNode(thisName);
        FnCallNode *propnode = newFnCallNode((INode*)thisnode, 0);
        if (lval->tag != NameUseTag)
            errorMsgNode(lval, ErrorNoMbr, "A name must be specified before the ':'");
        propnode->methprop = (NameUseNode*)lval;
        return (INode*)newAssignNode(NormalAssign, (INode*)propnode, rval);
    }
    else if (lexIsToken(LtltToken)) {
        FnCallNode *node = newFnCallOp(lval, "<<", 2);
        lexNextToken();
        nodesAdd(&node->args, parseAnyExpr(parse));
        return (INode*)node;
    }
	else
		return lval;
}

INode *parseList(ParseState *parse, INode *typenode) {
    FnCallNode *list = newFnCallNode(NULL, 4);
    list->tag = TypeLitTag;
    ArrayNode *arrtype = newArrayNode();
    list->objfn = typenode;
    list->vtype = (INode*)arrtype;
    lexNextToken();
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

// This parses any kind of expression, including blocks, asssignment or tuple
INode *parseAnyExpr(ParseState *parse) {
	switch (lex->toktype) {
	case IfToken:
		return parseIf(parse);
    case DoToken:
        lexNextToken();
        return parseBlock(parse);
	case LCurlyToken:
		return parseBlock(parse);
	default:
        return parseAssign(parse);
	}
}
