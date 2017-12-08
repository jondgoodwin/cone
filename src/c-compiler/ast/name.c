/** AST handling for names
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"
#include "../shared/error.h"

// Create a new name use node
NameUseAstNode *newNameUseNode(Symbol *namesym) {
	NameUseAstNode *name;
	newAstNode(name, NameUseAstNode, NameUseNode);
	name->namesym = namesym;
	return name;
}

// Serialize the AST for a name use
void nameUsePrint(int indent, NameUseAstNode *name) {
	astPrintLn(indent, "%s name reference", name->namesym->namestr);
}

// Check the name use's AST
void nameUsePass(AstPass *pstate, NameUseAstNode *name) {
}


// Create a new name declaraction node
NameDclAstNode *newNameDclNode(Symbol *namesym, AstNode *type, AstNode *perm) {
	NameDclAstNode *name;
	newAstNode(name, NameDclAstNode, NameDclNode);
	name->vtype = type;
	name->perm = (PermTypeAstNode *)immPerm;
	name->namesym = namesym;
	name->value = NULL;
	name->prev = NULL;
	name->scope = 0;
	name->index = 0;
	return name;
}

// Serialize the AST for a variable/function
void nameDclPrint(int indent, NameDclAstNode *name) {
	astPrintLn(indent, name->vtype->asttype == FnSig ? "fn %s()" : "var %s", name->namesym->namestr);
	astPrintNode(indent + 1, name->vtype, "");
	if (name->value)
		astPrintNode(indent + 1, name->value, "");
}

// Add name declaration to global namespace if it does not conflict or dupe implementation with prior definition
void nameDclGlobalPass(NameDclAstNode *NameDclNode) {
	Symbol *namesym = NameDclNode->namesym;

	// Remember function in symbol table, but error out if prior name has a different type
	// or both this and saved node define an implementation
	if (!namesym->node)
		namesym->node = (AstNode*)NameDclNode;
	else if (!typeEqual((AstNode*)NameDclNode, namesym->node)) {
		errorMsgNode((AstNode *)NameDclNode, ErrorTypNotSame, "Name is already defined with a different type/signature.");
		errorMsgNode(namesym->node, ErrorTypNotSame, "This is the conflicting definition for that name.");
	}
	else if (NameDclNode->value) {
		if (((NameDclAstNode*)namesym->node)->value) {
			errorMsgNode((AstNode *)NameDclNode, ErrorDupImpl, "Name has a duplicate implementation/value. Only one allowed.");
			errorMsgNode(namesym->node, ErrorDupImpl, "This is the other implementation/value.");
		}
		else
			namesym->node = (AstNode*)NameDclNode;
	}
}

// Syntactic sugar: Turn last statement implicit returns into explicit returns
void fnImplicitReturn(AstNode *rettype, BlockAstNode *blk) {
	AstNode *laststmt;
	laststmt = nodesGet(blk->nodes, blk->nodes->used - 1);
	if (rettype == voidType) {
		if (laststmt->asttype != ReturnNode)
			nodesAdd(&blk->nodes, (AstNode*) newReturnNode());
	}
	else {
		if (laststmt->asttype == StmtExpNode)
			laststmt->asttype = ReturnNode;
	}
}

// Check the name declaration's AST
void nameDclPass(AstPass *pstate, NameDclAstNode *name) {
	switch (pstate->pass) {
	case GlobalPass:
		nameDclGlobalPass(name);
		return;
	case TypeCheck:
		// Syntactic sugar: Turn implicit returns into explicit returns
		if (name->vtype->asttype == FnSig && name->value && name->value->asttype == BlockNode)
			fnImplicitReturn(((FnSigAstNode*)name->vtype)->rettype, (BlockAstNode *)name->value);
		break;
	}

	if (name->value)
		astPass(pstate, name->value);
}
