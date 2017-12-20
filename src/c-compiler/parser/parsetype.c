/** Parse type signatures
 * @file
 *
 * The parser translates the lexer's tokens into AST nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "../shared/symbol.h"
#include "lexer.h"

#include <stdio.h>
#include <assert.h>

// Parse a variable declaration
NameDclAstNode *parseVarDcl(PermAstNode *defperm) {
	Symbol *namesym = NULL;
	AstNode *vtype;
	PermAstNode *perm;
	AstNode *val;

	// Grab the permission type, if there
	assert(lexIsToken(IdentToken));
	if (lex->val.ident->node && lex->val.ident->node->asttype == PermNameDclNode) {
		perm = (PermAstNode*)((NameDclAstNode *)lex->val.ident->node)->value;
		lexNextToken();
	}
	else
		perm = defperm;

	// Obtain variable's name
	if (lexIsToken(IdentToken)) {
		namesym = lex->val.ident;
		lexNextToken();
	}
	else {
		errorMsgLex(ErrorNoIdent, "Expected variable name for declaration");
		return newNameDclNode(symFind("_",1), VarNameDclNode, voidType, perm, NULL);
	}

	// Get value type, if provided
	if ((vtype = parseVtype()) == NULL)
		vtype = voidType;

	// Get initialization value after '=', if provided
	if (lexIsToken(AssgnToken)) {
		lexNextToken();
		val = parseExp();
	}
	else
		val = NULL;

	return newNameDclNode(namesym, VarNameDclNode, vtype, perm, val);
}

// Parse a function's type signature
AstNode *parseFnSig() {
	FnSigAstNode *fnsig;
	Symbol *namesym = NULL;

	// Skip past the 'fn'
	lexNextToken();

	// Process function name, if provided
	if (lexIsToken(IdentToken)) {
		namesym = lex->val.ident;
		lexNextToken();
	}

	// Set up memory block for the function's type signature
	fnsig = newFnSigNode();

	// Process parameter declarations
	if (lexIsToken(LParenToken)) {
		lexNextToken();
		while (lexIsToken(IdentToken)) {
			NameDclAstNode *parm = parseVarDcl(constPerm);
			inodesAdd(&fnsig->parms, parm->namesym, (AstNode*)parm);
			if (!lexIsToken(CommaToken))
				break;
			lexNextToken();
		}
		parseRParen();
	}
	else
		errorMsgLex(ErrorNoLParen, "Expected left parenthesis for parameter declarations");

	// Parse return type info - turn into void if none specified
	if ((fnsig->rettype = parseVtype())==NULL) {
		fnsig->rettype = voidType;
	}

	if (namesym == NULL)
		return (AstNode*)fnsig;
	else
		return (AstNode*)newNameDclNode(namesym, VarNameDclNode, (AstNode*)fnsig, immPerm, NULL);
}

// Parse a value type signature. Return NULL if none found.
AstNode* parseVtype() {
	AstNode *vtype;
	switch (lex->toktype) {
	case IdentToken:
		vtype = (AstNode*)newNameUseNode(lex->val.ident);
		lexNextToken();
		return vtype;
	default:
		return NULL;
	}
}
