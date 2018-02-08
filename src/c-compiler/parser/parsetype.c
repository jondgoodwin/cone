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

// Parse a permission, return defperm if not found
PermAstNode *parsePerm(PermAstNode *defperm) {
	if (lexIsToken(IdentToken)
		&& lex->val.ident->node && lex->val.ident->node->asttype == PermNameDclNode) {
		PermAstNode *perm;
		perm = (PermAstNode*)((NameDclAstNode *)lex->val.ident->node)->value;
		lexNextToken();
		return perm;
	}
	else
		return defperm;
}

// Parse a variable declaration
NameDclAstNode *parseVarDcl(PermAstNode *defperm) {
	Symbol *namesym = NULL;
	AstNode *vtype;
	PermAstNode *perm;
	AstNode *val;

	// Grab the permission type
	perm = parsePerm(defperm);

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
		val = parseExpr();
	}
	else
		val = NULL;

	return newNameDclNode(namesym, VarNameDclNode, vtype, perm, val);
}

// Parse a struct
AstNode *parseStruct() {
	NameDclAstNode *strdclnode;
	StructAstNode *strnode;
	int16_t fieldnbr = 0;

	strnode = newStructNode();
	strdclnode = newNameDclNode(NULL, VtypeNameDclNode, voidType, immPerm, (AstNode*)strnode);

	// Skip past 'struct'
	lexNextToken();

	// Process struct type name, if provided
	if (lexIsToken(IdentToken)) {
		strdclnode->namesym = lex->val.ident;
		lexNextToken();
	}

	// Process field or method definitions
	if (lexIsToken(LCurlyToken)) {
		lexNextToken();
		while (1) {
			if (lexIsToken(FnToken)) {
				NameDclAstNode *fn = (NameDclAstNode *)parseFn();
				nodesAdd(&strnode->methods, (AstNode*)fn);
			}
			else if (lexIsToken(IdentToken)) {
				NameDclAstNode *field = parseVarDcl(mutPerm);
				field->scope = 1;
				field->index = fieldnbr++;
				inodesAdd(&strnode->fields, field->namesym, (AstNode*)field);
				if (!lexIsToken(SemiToken))
					break;
				lexNextToken();
			}
			else
				break;
		}
		parseRCurly();
	}
	else
		errorMsgLex(ErrorNoLCurly, "Expected left curly for struct's fields");

	return (AstNode*)strdclnode;
}

// Parse a function's type signature
AstNode *parseFnSig() {
	FnSigAstNode *fnsig;
	Symbol *namesym = NULL;
	int16_t parmnbr = 0;

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
		int usesDefaults = 0;
		lexNextToken();
		while (lexIsToken(IdentToken)) {
			NameDclAstNode *parm = parseVarDcl(immPerm);
			parm->scope = 1;
			parm->index = parmnbr++;
			if (usesDefaults && parm->value == NULL)
				errorMsgNode((AstNode*)parm, ErrorNoInit, "Must specify default value since prior parm did");
			else if (parm->value)
				usesDefaults = 1;
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

// Parse a pointer type
AstNode *parsePtrType() {
	PtrAstNode *ptype = newPtrTypeNode();
	lexNextToken();

	// For now - no allocator may be specified
	ptype->alloc = voidType;	// default is borrowed reference

	// Get permission, if specified (default is const)
	if (lexIsToken(IdentToken) && lex->val.ident->node && lex->val.ident->node->asttype == PermNameDclNode) {
		ptype->perm = (PermAstNode*)((NameDclAstNode *)lex->val.ident->node)->value;
		lexNextToken();
	}
	else
		ptype->perm = constPerm;

	// Get value type, if provided
	if (lexIsToken(FnToken)) {
		ptype->pvtype = parseFnSig();
	}
	else if ((ptype->pvtype = parseVtype()) == NULL) {
		errorMsgLex(ErrorNoVtype, "Missing value type for the pointer");
		ptype->pvtype = voidType;
	}

	return (AstNode *)ptype;
}

// Parse an array type
AstNode *parseArrayType() {
	ArrayAstNode *atype = newArrayNode();
	lexNextToken();

	atype->size = 0;
	lexNextToken(); // closing bracket - assume no size for now

	if ((atype->elemtype = parseVtype()) == NULL) {
		errorMsgLex(ErrorNoVtype, "Missing value type for the array element");
		atype->elemtype = voidType;
	}

	return (AstNode *)atype;
}

// Parse a value type signature. Return NULL if none found.
AstNode* parseVtype() {
	AstNode *vtype;
	switch (lex->toktype) {
	case AmperToken:
		return parsePtrType();
	case LBracketToken:
		return parseArrayType();
	case IdentToken:
		vtype = (AstNode*)newNameUseNode(lex->val.ident);
		lexNextToken();
		return vtype;
	default:
		return voidType;
	}
}
