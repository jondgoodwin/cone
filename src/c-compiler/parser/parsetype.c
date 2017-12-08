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
		if (lexIsToken(RParenToken))
			lexNextToken();
		else
			errorMsgLex(ErrorNoRParen, "Expected right parenthesis that ends parameters");
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
		return (AstNode*)newNameDclNode(namesym, (AstNode*)fnsig, immPerm, NULL);
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

// Parse a permission type. Return NULL if not found.
AstNode* parsePerm() {
	switch (lex->toktype) {
	case IdentToken:
		return (AstNode*) newNameUseNode(lex->val.ident);
	default:
		return NULL;
	}
}

