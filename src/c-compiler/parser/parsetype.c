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
#include "../ast/nametbl.h"
#include "lexer.h"

#include <stdio.h>
#include <assert.h>

// Parse a permission, return defperm if not found
PermAstNode *parsePerm(PermAstNode *defperm) {
	if (lexIsToken(PermToken)) {
		PermAstNode *perm;
		perm = (PermAstNode*)lex->val.ident->node;
		lexNextToken();
		return perm;
	}
	else
		return defperm;
}

// Parse an allocator + permission for a reference type
void parseAllocPerm(PtrAstNode *refnode) {
	if (lexIsToken(IdentToken)
		&& lex->val.ident->node && lex->val.ident->node->asttype == AllocType) {
		refnode->alloc = (AstNode*)lex->val.ident->node;
		lexNextToken();
		refnode->perm = parsePerm(uniPerm);
	}
	else {
		refnode->alloc = voidType;
		refnode->perm = parsePerm(constPerm);
	}
}

// Parse a variable declaration
VarDclAstNode *parseVarDcl(ParseState *parse, PermAstNode *defperm, uint16_t flags) {
	VarDclAstNode *varnode;
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
		return newVarDclNode(nametblFind("_",1), VarNameDclNode, voidType, perm, NULL);
	}

	// Get value type, if provided
	if ((vtype = parseVtype(parse)) == NULL)
		vtype = voidType;

	// Get initialization value after '=', if provided
	if (lexIsToken(AssgnToken)) {
		if (!(flags&ParseMayImpl))
			errorMsgLex(ErrorBadImpl, "A default/initial value may not be specified here.");
		lexNextToken();
		val = parseExpr(parse);
	}
	else {
		if (!(flags&ParseMaySig))
			errorMsgLex(ErrorNoInit, "Must specify default/initial value.");
		val = NULL;
	}

	varnode = newVarDclNode(namesym, VarNameDclNode, vtype, perm, val);
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
		lexNextToken();
		if (lexIsToken(IdentToken)) {
			errorMsgLex(WarnName, "Unnecessary name is ignored");
			lexNextToken();
		}
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
	FieldsAstNode *strnode;
	int16_t fieldnbr = 0;

    // Capture the kind of type, then get next token (name)
    uint16_t tag = lexIsToken(AllocToken) ? AllocType : StructType;
    lexNextToken();

    // Process struct type name, if provided
    if (lexIsToken(IdentToken)) {
        strnode = newStructNode(lex->val.ident);
        strnode->asttype = tag;
        strnode->owner = parse->owner;
        parse->owner = (NamedAstNode *)strnode;
        lexNextToken();
    }
    else {
        errorMsgLex(ErrorNoIdent, "Expected a name for the type");
        return NULL;
    }

	// Process field or method definitions
	if (lexIsToken(LCurlyToken)) {
		lexNextToken();
		while (1) {
			if (lexIsToken(FnToken)) {
				VarDclAstNode *fn = (VarDclAstNode *)parseFn(parse, ParseMayName | ParseMayImpl);
				fn->flags |= FlagMangleParms;
                if (fn && isNamedNode(fn))
                    namespaceAddFnTuple(&strnode->methfields, (NamedAstNode*)fn);
			}
			else if (lexIsToken(PermToken) || lexIsToken(IdentToken)) {
				VarDclAstNode *field = parseVarDcl(parse, mutPerm, ParseMayImpl | ParseMaySig);
				field->scope = 1;
				field->index = fieldnbr++;
				nodesAdd(&strnode->fields, (AstNode*)field);
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
	return (AstNode*)strnode;
}

void parseInjectSelf(FnSigAstNode *fnsig, Name *typename) {
	NameUseAstNode *selftype = newNameUseNode(typename);
	VarDclAstNode *selfparm = newVarDclNode(nametblFind("self", 4), VarNameDclNode, (AstNode*)selftype, constPerm, NULL);
	selfparm->scope = 1;
	selfparm->index = 0;
	nodesAdd(&fnsig->parms, (AstNode*)selfparm);
}

// Parse a function's type signature
AstNode *parseFnSig(ParseState *parse) {
	FnSigAstNode *fnsig;
	int16_t parmnbr = 0;
	uint16_t parseflags = ParseMaySig | ParseMayImpl;

	// Set up memory block for the function's type signature
	fnsig = newFnSigNode();

	// Process parameter declarations
	if (lexIsToken(LParenToken)) {
		lexNextToken();
		// A type's method with no parameters should still define self
		if (lexIsToken(RParenToken) && isTypeNode(parse->owner))
			parseInjectSelf(fnsig, parse->owner->namesym);
		while (lexIsToken(PermToken) || lexIsToken(IdentToken)) {
			VarDclAstNode *parm = parseVarDcl(parse, immPerm, parseflags);
			// Do special inference if function is a type's method
			if (isTypeNode(parse->owner)) {
				// Create default self parm, if 'self' was not specified
				if (parmnbr == 0 && parm->namesym != nametblFind("self", 4)) {
					parseInjectSelf(fnsig, parse->owner->namesym);
					++parmnbr;
				}
				// Infer value type of a parameter (or its reference) if unspecified
				if (parm->vtype == voidType) {
					parm->vtype = (AstNode*)newNameUseNode(parse->owner->namesym);
				}
				else if (parm->vtype->asttype == RefType) {
					PtrAstNode *refnode = (PtrAstNode *)parm->vtype;
					if (refnode->pvtype == voidType) {
						refnode->pvtype = (AstNode*)newNameUseNode(parse->owner->namesym);
					}
				}
			}
			// Add parameter to function's parm list
			parm->scope = 1;
			parm->index = parmnbr++;
			if (parm->value)
				parseflags = ParseMayImpl; // force remaining parms to specify default
			nodesAdd(&fnsig->parms, (AstNode*)parm);
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

	return (AstNode*)fnsig;
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
