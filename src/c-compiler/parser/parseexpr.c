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
			node = parseExpr(parse);
			parseRParen();
			return node;
		}
	default:
		errorMsgLex(ErrorBadTerm, "Invalid term value: expected variable, literal, etc.");
		return NULL;
	}
}

// Parse the postfix operators: '.', '::', '()'
INode *parsePostfix(ParseState *parse) {
	INode *node = parseTerm(parse);
	while (1) {
		switch (lex->toktype) {

		// Function call with possible parameters
		case LParenToken:
		{
			FnCallNode *fncall = newFnCallNode(node, 8);
			lexNextToken();
			if (!lexIsToken(RParenToken))
				while (1) {
					nodesAdd(&fncall->args, parseExpr(parse));
					if (lexIsToken(CommaToken))
						lexNextToken();
					else
						break;
				}
			parseRParen();
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
						nodesAdd(&fncall->args, parseExpr(parse));
						if (lexIsToken(CommaToken))
							lexNextToken();
						else
							break;
					}
				}
				parseRParen();
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

	// Address node's value type is a partially populated pointer type
	PtrNode *ptype = newPtrTypeNode();
	ptype->pvtype = NULL;     // Type inference will correct this
	parseAllocPerm(ptype);
	anode->vtype = (INode *)ptype;

	// A value or constructor
	anode->exp = parseTerm(parse);

	return (INode *)anode;
}

// Parse a prefix operator, e.g.: -
INode *parsePrefix(ParseState *parse) {
	switch (lex->toktype) {
	case DashToken:
	{
		FnCallNode *node = newFnCallOp(NULL, "neg", 0);
		lexNextToken();
		node->objfn = parsePrefix(parse);
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
INode *parseOrLogic(ParseState *parse) {
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

// Parse an assignment expression
INode *parseAssign(ParseState *parse) {
	INode *lval = parseOrLogic(parse);
	if (lexIsToken(AssgnToken)) {
		lexNextToken();
		INode *rval = parseExpr(parse);
		return (INode*) newAssignNode(NormalAssign, lval, rval);
	}
	else
		return lval;
}

INode *parseExpBlock(ParseState *parse) {
	switch (lex->toktype) {
	case IfToken:
		return parseIf(parse);
	case LCurlyToken:
		return parseBlock(parse);
	default:
		return parseAssign(parse);
	}
}

// Parse an expression
INode *parseExpr(ParseState *parse) {
	return parseExpBlock(parse);
}
