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

	// Process return type info
	fnsig->rettype = parseType();
}

// Parse a single type sequence
LangTypeInfo* parseType() {
	switch (lex->toktype) {
	case IdentToken:
		{
		TypeAstNode *inode = (TypeAstNode*)lex->val.ident->node;
		if (inode->asttype==TypeNode) {
			lexNextToken();
			return inode->type;
		} else
			return voidType;
		}
	case FnToken:
		{
		TypeAndName typnam;
		parseFnType(&typnam);
		return (LangTypeInfo*) typnam.typeinfo;
		}
	default:
		return voidType;
	}
}

// Parse a multi-dimensional type for an expression's value
QuadTypeInfo *parseQuadType() {
	QuadTypeInfo *quad;
	LangTypeInfo *typ;
	quad = (QuadTypeInfo*)memAllocBlk(sizeof(QuadTypeInfo));
	quad->valtype = voidType;
	quad->permtype = voidType;
	quad->alloctype = voidType;
	quad->lifetype = voidType;

	while (1) {
		typ = parseType();
		if (typ == voidType) {
			// Not a type: If it's a name, pick it up, else return
			if (lexIsToken(IdentToken))
				quad->name = lex->val.ident;
			else
				return quad;
		}
		else {
			// Plug the type info in the correct unoccupied slot
			if (typ->type == PermType) {
				if (quad->permtype==voidType)
					quad->permtype = typ;
				else
					errorMsgLex(ErrorDupType, "May not specify permissions more than once");
			} else if (typ->type == AllocType) {
				if (quad->alloctype==voidType)
					quad->alloctype = typ;
				else
					errorMsgLex(ErrorDupType, "May not specify permissions more than once");
			} else {
				if (quad->valtype==voidType)
					quad->valtype = typ;
				else
					errorMsgLex(ErrorDupType, "May not specify permissions more than once");
			}
		}
	}
}