/** Parse type signatures
 * @file
 *
 * The parser translates the lexer's tokens into AST nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "parser.h"
#include "../shared/ast.h"
#include "../shared/memory.h"
#include "../shared/error.h"
#include "lexer.h"

#include <stdio.h>

// Parse a function's type signature
void parseFnType(TypeAndName *typnam) {
	FnTypeInfo *fnsig;

	// Skip past the 'fn'
	lexNextToken();

	// Process function name, if provided
	if (lexIsToken(IdentToken)) {
		typnam->symname = lex->val.ident;
		lexNextToken();
	}
	else {
		// For anonymous function, create and populate fake symbol table entry
		typnam->symname = NULL;
	}

	// Set up memory block for the function's type signature
	fnsig = typnam->typeinfo = (FnTypeInfo*) memAllocBlk(sizeof(FnTypeInfo));
	fnsig->type = FnType;
	fnsig->flags = 0;
	fnsig->rettype = NULL;

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
}
