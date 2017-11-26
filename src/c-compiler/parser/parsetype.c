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
void parseFnType(TypeAndName *typnam) {
	FnTypeAstNode *fnsig;

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
	fnsig = typnam->TypeAstNode = (FnTypeAstNode*) memAllocBlk(sizeof(FnTypeAstNode));
	fnsig->asttype = FnType;
	fnsig->flags = 0;

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
	if ((fnsig->rettype = parseType())==NULL) {
		VoidTypeAstNode *voidtype;
		newAstNode(voidtype, VoidTypeAstNode, VoidType);
		fnsig->rettype = (AstNode*)voidtype;
	}

}

// Parse a single type sequence
AstNode* parseType() {
	switch (lex->toktype) {
	case IdentToken:
		{
		TypeAstNode *inode = (TypeAstNode*)lex->val.ident->node;
		if (inode->asttype==TypeNode) {
			lexNextToken();
			return inode->type;
		} else
			return NULL;
		}
	case FnToken:
		{
		TypeAndName typnam;
		parseFnType(&typnam);
		return (AstNode*) typnam.TypeAstNode;
		}
	default:
		return NULL;
	}
}
