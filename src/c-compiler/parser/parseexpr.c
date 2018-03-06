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
#include "../shared/name.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "lexer.h"

#include <stdio.h>
#include <assert.h>

AstNode *parseAddr(ParseState *parse);

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
		{
			NameUseAstNode *node = newNameUseNode(lex->val.ident);
			lexNextToken();
			return (AstNode*)node;
		}
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
					nodesAdd(&fncall->parms, parseExpr(parse));
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
			lexNextToken();
			// Get field/method name
			if (!lexIsToken(IdentToken)) {
				errorMsgLex(ErrorNoMbr, "This should be a named field/method");
				lexNextToken();
				break;
			}
			AstNode *method = (AstNode*)newNameUseNode(lex->val.ident);
			lexNextToken();
			method->asttype = MemberUseNode;
			
			// If parameters provided, make this a function call
			// (where MemberUseNode signals it is an OO call)
			if (lexIsToken(LParenToken)) {
				lexNextToken();
				FnCallAstNode *fncall = newFnCallAstNode(method, 8);
				nodesAdd(&fncall->parms, node); // treat object as first parameter (self)
				if (!lexIsToken(RParenToken)) {
					while (1) {
						nodesAdd(&fncall->parms, parseExpr(parse));
						if (lexIsToken(CommaToken))
							lexNextToken();
						else
							break;
					}
				}
				parseRParen();
				node = (AstNode *)fncall;
			}
			else {
				ElementAstNode *elem = newElementAstNode();
				elem->owner = node;
				elem->element = method;
				node = (AstNode *)elem;
			}
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
		FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("neg", 3)), 1);
		lexNextToken();
		nodesAdd(&node->parms, parsePrefix(parse));
		return (AstNode *)node;
	}
	case TildeToken:
	{
		FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("~", 1)), 1);
		lexNextToken();
		nodesAdd(&node->parms, parsePrefix(parse));
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
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("*", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseCast(parse));
			lhnode = (AstNode*)node;
		}
		else if (lexIsToken(SlashToken)) {
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("/", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseCast(parse));
			lhnode = (AstNode*)node;
		}
		else if (lexIsToken(PercentToken)) {
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("%", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseCast(parse));
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
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("+", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseMult(parse));
			lhnode = (AstNode*)node;
		}
		else if (lexIsToken(DashToken)) {
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("-", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseMult(parse));
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
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("&", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseAdd(parse));
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
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("^", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseAnd(parse));
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
			FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind("|", 1)), 2);
			lexNextToken();
			nodesAdd(&node->parms, lhnode);
			nodesAdd(&node->parms, parseXor(parse));
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
	int cmpsz = 2;

	switch (lex->toktype) {
	case EqToken:  cmpop = "=="; break;
	case NeToken:  cmpop = "!="; break;
	case LtToken:  cmpop = "<"; cmpsz = 1; break;
	case LeToken:  cmpop = "<="; break;
	case GtToken:  cmpop = ">"; cmpsz = 1; break;
	case GeToken:  cmpop = ">="; break;
	default: return lhnode;
	}

	FnCallAstNode *node = newFnCallAstNode((AstNode*)newMemberUseNode(nameFind(cmpop, cmpsz)), 2);
	lexNextToken();
	nodesAdd(&node->parms, lhnode);
	nodesAdd(&node->parms, parseOr(parse));
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
