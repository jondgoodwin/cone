/** Parse expressions
 * @file
 *
 * The parser translates the lexer's tokens into AST nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ast/ast.h"
#include "../ast/nametbl.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "lexer.h"

#include <stdio.h>
#include <assert.h>

AstNode *parseAddr(ParseState *parse);

// Parse a name use, which may be qualified with module names
AstNode *parseNameUse(ParseState *parse) {
    NameUseAstNode *nameuse = newNameUseNode(NULL);

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
    return (AstNode*)nameuse;
}

// Parse a term: literal, identifier, etc.
AstNode *parseTerm(ParseState *parse) {
	switch (lex->toktype) {
	case trueToken:
	{
		ULitAstNode *node = newULitNode(1, (AstNode*)boolType);
		lexNextToken();
		return (AstNode *)node;
	}
	case falseToken:
	{
		ULitAstNode *node = newULitNode(0, (AstNode*)boolType);
		lexNextToken();
		return (AstNode *)node;
	}
	case IntLitToken:
		{
			ULitAstNode *node = newULitNode(lex->val.uintlit, lex->langtype);
			lexNextToken();
			return (AstNode *)node;
		}
	case FloatLitToken:
		{
			FLitAstNode *node = newFLitNode(lex->val.floatlit, lex->langtype);
			lexNextToken();
			return (AstNode *)node;
		}
	case StrLitToken:
		{
			SLitAstNode *node = newSLitNode(lex->val.strlit, lex->langtype);
			lexNextToken();
			return (AstNode *)node;
		}
	case IdentToken:
	case DblColonToken:
		return (AstNode*)parseNameUse(parse);
	case LParenToken:
		{
			AstNode *node;
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
AstNode *parsePostfix(ParseState *parse) {
	AstNode *node = parseTerm(parse);
	while (1) {
		switch (lex->toktype) {

		// Function call with possible parameters
		case LParenToken:
		{
			FnCallAstNode *fncall = newFnCallAstNode(node, 8);
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
			node = (AstNode *)fncall;
			break;
		}

		// Object call with possible parameters
		case DotToken:
		{
            FnCallAstNode *fncall = newFnCallAstNode(node, 0);
            lexNextToken();

			// Get property/method name
			if (!lexIsToken(IdentToken)) {
				errorMsgLex(ErrorNoMbr, "This should be a named property/method");
				lexNextToken();
				break;
			}
			NameUseAstNode *method = newNameUseNode(lex->val.ident);
			method->asttype = MbrNameUseTag;
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
            node = (AstNode *)fncall;
            break;
		}

		default:
			return node;
		}
	}
}

// Parse an address term - current token is '&'
AstNode *parseAddr(ParseState *parse) {
	AddrAstNode *anode = newAddrAstNode();
	lexNextToken();

	// Address node's value type is a partially populated pointer type
	PtrAstNode *ptype = newPtrTypeNode();
	ptype->pvtype = NULL;     // Type inference will correct this
	parseAllocPerm(ptype);
	anode->vtype = (AstNode *)ptype;

	// A value or constructor
	anode->exp = parseTerm(parse);

	return (AstNode *)anode;
}

// Parse a prefix operator, e.g.: -
AstNode *parsePrefix(ParseState *parse) {
	switch (lex->toktype) {
	case DashToken:
	{
		FnCallAstNode *node = newFnCallOp(NULL, "neg", 0);
		lexNextToken();
		node->objfn = parsePrefix(parse);
		return (AstNode *)node;
	}
	case TildeToken:
	{
		FnCallAstNode *node = newFnCallOp(NULL, "~", 0);
		lexNextToken();
		node->objfn = parsePrefix(parse);
		return (AstNode *)node;
	}
	case StarToken:
	{
		DerefAstNode *node = newDerefAstNode();
		lexNextToken();
		node->exp = parsePrefix(parse);
		return (AstNode *)node;
	}
	case AmperToken:
		return parseAddr(parse);
	default:
		return parsePostfix(parse);
	}
}

// Parse type cast
AstNode *parseCast(ParseState *parse) {
	AstNode *lhnode = parsePrefix(parse);
	if (lexIsToken(AsToken)) {
		CastAstNode *node = newCastAstNode(lhnode, NULL);
		lexNextToken();
		node->vtype = parseVtype(parse);
		return (AstNode*)node;
	}
	return lhnode;
}

// Parse binary multiply, divide, rem operator
AstNode *parseMult(ParseState *parse) {
	AstNode *lhnode = parseCast(parse);
	while (1) {
		if (lexIsToken(StarToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "*", 2);
			lexNextToken();
			nodesAdd(&node->args, parseCast(parse));
			lhnode = (AstNode*)node;
		}
		else if (lexIsToken(SlashToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "/", 2);
			lexNextToken();
			nodesAdd(&node->args, parseCast(parse));
			lhnode = (AstNode*)node;
		}
		else if (lexIsToken(PercentToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "%", 2);
			lexNextToken();
			nodesAdd(&node->args, parseCast(parse));
			lhnode = (AstNode*)node;
		}
		else
			return lhnode;
	}
}

// Parse binary add, subtract operator
AstNode *parseAdd(ParseState *parse) {
	AstNode *lhnode = parseMult(parse);
	while (1) {
		if (lexIsToken(PlusToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "+", 2);
			lexNextToken();
			nodesAdd(&node->args, parseMult(parse));
			lhnode = (AstNode*)node;
		}
		else if (lexIsToken(DashToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "-", 2);
			lexNextToken();
			nodesAdd(&node->args, parseMult(parse));
			lhnode = (AstNode*)node;
		}
		else
			return lhnode;
	}
}

// Parse bitwise And
AstNode *parseAnd(ParseState *parse) {
	AstNode *lhnode = parseAdd(parse);
	while (1) {
		if (lexIsToken(AmperToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "&", 2);
			lexNextToken();
			nodesAdd(&node->args, parseAdd(parse));
			lhnode = (AstNode*)node;
		}
		else
			return lhnode;
	}
}

// Parse bitwise Xor
AstNode *parseXor(ParseState *parse) {
	AstNode *lhnode = parseAnd(parse);
	while (1) {
		if (lexIsToken(CaretToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "^", 2);
			lexNextToken();
			nodesAdd(&node->args, parseAnd(parse));
			lhnode = (AstNode*)node;
		}
		else
			return lhnode;
	}
}

// Parse bitwise or
AstNode *parseOr(ParseState *parse) {
	AstNode *lhnode = parseXor(parse);
	while (1) {
		if (lexIsToken(BarToken)) {
			FnCallAstNode *node = newFnCallOp(lhnode, "|", 2);
			lexNextToken();
			nodesAdd(&node->args, parseXor(parse));
			lhnode = (AstNode*)node;
		}
		else
			return lhnode;
	}
}

// Parse comparison operator
AstNode *parseCmp(ParseState *parse) {
	AstNode *lhnode = parseOr(parse);
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

	FnCallAstNode *node = newFnCallOp(lhnode, cmpop, 2);
	lexNextToken();
	nodesAdd(&node->args, parseOr(parse));
	return (AstNode*)node;
}

// Parse 'not' logical operator
AstNode *parseNotLogic(ParseState *parse) {
	if (lexIsToken(NotToken)) {
		LogicAstNode *node = newLogicAstNode(NotLogicNode);
		lexNextToken();
		node->lexp = parseNotLogic(parse);
		return (AstNode*)node;
	}
	return parseCmp(parse);
}

// Parse 'and' logical operator
AstNode *parseAndLogic(ParseState *parse) {
	AstNode *lhnode = parseNotLogic(parse);
	while (lexIsToken(AndToken)) {
		LogicAstNode *node = newLogicAstNode(AndLogicNode);
		lexNextToken();
		node->lexp = lhnode;
		node->rexp = parseNotLogic(parse);
		lhnode = (AstNode*)node;
	}
	return lhnode;
}

// Parse 'or' logical operator
AstNode *parseOrLogic(ParseState *parse) {
	AstNode *lhnode = parseAndLogic(parse);
	while (lexIsToken(OrToken)) {
		LogicAstNode *node = newLogicAstNode(OrLogicNode);
		lexNextToken();
		node->lexp = lhnode;
		node->rexp = parseAndLogic(parse);
		lhnode = (AstNode*)node;
	}
	return lhnode;
}

// Parse an assignment expression
AstNode *parseAssign(ParseState *parse) {
	AstNode *lval = parseOrLogic(parse);
	if (lexIsToken(AssgnToken)) {
		lexNextToken();
		AstNode *rval = parseExpr(parse);
		return (AstNode*) newAssignAstNode(NormalAssign, lval, rval);
	}
	else
		return lval;
}

AstNode *parseExpBlock(ParseState *parse) {
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
AstNode *parseExpr(ParseState *parse) {
	return parseExpBlock(parse);
}
