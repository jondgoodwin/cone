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
	Symbol *name;

	// Skip past the 'fn'
	lexNextToken();

	// Process function name, if provided
	if (lexIsToken(IdentToken)) {
		name = lex->val.ident;
		lexNextToken();
	}
	else {
		// For anonymous function, create and populate fake symbol table entry
		name = NULL;
	}

	// Set up memory block for the function's type signature
	fnsig = newFnSigNode();
	fnsig->name = name;

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
		fnsig->rettype = voidType;
	}

	return (AstNode*) fnsig;
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
		return (AstNode*) parseFnSig();
	case i8Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(IntType, 1, lex->val.ident); lexNextToken(); return node;}
	case i16Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(IntType, 2, lex->val.ident); lexNextToken(); return node;}
	case i32Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(IntType, 4, lex->val.ident); lexNextToken(); return node;}
	case i64Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(IntType, 8, lex->val.ident); lexNextToken(); return node;}
	case u8Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(UintType, 1, lex->val.ident); lexNextToken(); return node;}
	case u16Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(UintType, 2, lex->val.ident); lexNextToken(); return node;}
	case u32Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(UintType, 4, lex->val.ident); lexNextToken(); return node;}
	case u64Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(UintType, 8, lex->val.ident); lexNextToken(); return node;}
	case f32Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(FloatType, 4, lex->val.ident); lexNextToken(); return node;}
	case f64Token:
		{AstNode *node; node = (AstNode*) newNbrTypeNode(FloatType, 8, lex->val.ident); lexNextToken(); return node;}
	default:
		return NULL;
	}
}