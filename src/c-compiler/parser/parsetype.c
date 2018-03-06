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
#include "../shared/name.h"
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

// Parse an allocator + permission for a reference type
void parseAllocPerm(PtrAstNode *refnode) {
	if (lexIsToken(IdentToken)
		&& lex->val.ident->node && lex->val.ident->node->asttype == AllocNameDclNode) {
		refnode->alloc = ((NameDclAstNode *)lex->val.ident->node)->value;
		lexNextToken();
		refnode->perm = parsePerm(uniPerm);
	}
	else {
		refnode->alloc = voidType;
		refnode->perm = parsePerm(constPerm);
	}
}

// Parse a variable declaration
NameDclAstNode *parseVarDcl(ParseState *parse, PermAstNode *defperm) {
	NameDclAstNode *varnode;
	Name *namesym = NULL;
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
		return newNameDclNode(nameFind("_",1), VarNameDclNode, voidType, perm, NULL);
	}

	// Get value type, if provided
	if ((vtype = parseVtype(parse)) == NULL)
		vtype = voidType;

	// Get initialization value after '=', if provided
	if (lexIsToken(AssgnToken)) {
		lexNextToken();
		val = parseExpr(parse);
	}
	else
		val = NULL;

	varnode = newNameDclNode(namesym, VarNameDclNode, vtype, perm, val);
	varnode->owner = parse->owner;
	return varnode;
}

// Parse a pointer type
AstNode *parsePtrType(ParseState *parse) {
	PtrAstNode *ptype = newPtrTypeNode();
	if (lexIsToken(StarToken))
		ptype->asttype = PtrType;
	lexNextToken();

	// Get allocator/permission for references
	if (ptype->asttype == RefType)
		parseAllocPerm(ptype);
	else {
		ptype->alloc = voidType;	// no allocator
		ptype->perm = parsePerm(constPerm);
	}

	// Get value type, if provided
	if (lexIsToken(FnToken)) {
		ptype->pvtype = parseFnSig(parse);
	}
	else if ((ptype->pvtype = parseVtype(parse)) == NULL) {
		errorMsgLex(ErrorNoVtype, "Missing value type for the pointer");
		ptype->pvtype = voidType;
	}

	return (AstNode *)ptype;
}

// Parse a struct
AstNode *parseStruct(ParseState *parse) {
	NamedAstNode *svowner = parse->owner;
	NameDclAstNode *strdclnode;
	StructAstNode *strnode;
	int16_t fieldnbr = 0;

	strnode = newStructNode();
	strdclnode = newNameDclNode(NULL, VtypeNameDclNode, voidType, immPerm, (AstNode*)strnode);
	strdclnode->owner = parse->owner;
	parse->owner = (NamedAstNode *)strdclnode;
	if (lexIsToken(AllocToken)) {
		strnode->asttype = AllocType;
		strdclnode->asttype = AllocNameDclNode;
	}

	// Skip past 'struct'/'alloc'
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
				NameDclAstNode *fn = (NameDclAstNode *)parseFn(parse);
				fn->flags |= FlagMangleParms;
				nodesAdd(&strnode->methods, (AstNode*)fn);
			}
			else if (lexIsToken(IdentToken)) {
				NameDclAstNode *field = parseVarDcl(parse, mutPerm);
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
		errorMsgLex(ErrorNoLCurly, "Expected left curly bracket enclosing fields or methods");

	parse->owner = svowner;
	return (AstNode*)strdclnode;
}

// Parse a function's type signature
AstNode *parseFnSig(ParseState *parse) {
	FnSigAstNode *fnsig;
	Name *namesym = NULL;
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
			NameDclAstNode *parm = parseVarDcl(parse, immPerm);
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
	if ((fnsig->rettype = parseVtype(parse))==NULL) {
		fnsig->rettype = voidType;
	}

	if (namesym == NULL)
		return (AstNode*)fnsig;
	else
		return (AstNode*)newNameDclNode(namesym, VarNameDclNode, (AstNode*)fnsig, immPerm, NULL);
}

// Parse an array type
AstNode *parseArrayType(ParseState *parse) {
	ArrayAstNode *atype = newArrayNode();
	lexNextToken();

	atype->size = 0;
	lexNextToken(); // closing bracket - assume no size for now

	if ((atype->elemtype = parseVtype(parse)) == NULL) {
		errorMsgLex(ErrorNoVtype, "Missing value type for the array element");
		atype->elemtype = voidType;
	}

	return (AstNode *)atype;
}

// Parse a value type signature. Return NULL if none found.
AstNode* parseVtype(ParseState *parse) {
	AstNode *vtype;
	switch (lex->toktype) {
	case AmperToken: case StarToken:
		return parsePtrType(parse);
	case LBracketToken:
		return parseArrayType(parse);
	case IdentToken:
		vtype = (AstNode*)newNameUseNode(lex->val.ident);
		lexNextToken();
		return vtype;
	default:
		return voidType;
	}
}
