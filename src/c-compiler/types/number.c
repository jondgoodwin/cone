/** AST handling for primitive numbers
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../shared/symbol.h"

Inodes *utypenames;
Inodes *itypenames;
Inodes *ftypenames;
Nodes *nbrtraits;

// Declare built-in number types and their names
void nbrDclNames() {
	utypenames = newInodes(1);
	itypenames = newInodes(1);
	ftypenames = newInodes(1);

	nbrtraits = newNodes(8);	// Needs 'copy' etc.

	newNameDclNodeStr("i8", VtypeNameDclNode, (AstNode*)(i8Type = newNbrTypeNode(IntNbrType, 8)));
	newNameDclNodeStr("i16", VtypeNameDclNode, (AstNode*)(i16Type = newNbrTypeNode(IntNbrType, 16)));
	newNameDclNodeStr("i32", VtypeNameDclNode, (AstNode*)(i32Type = newNbrTypeNode(IntNbrType, 32)));
	newNameDclNodeStr("i64", VtypeNameDclNode, (AstNode*)(i64Type = newNbrTypeNode(IntNbrType, 64)));
	newNameDclNodeStr("u8", VtypeNameDclNode, (AstNode*)(u8Type = newNbrTypeNode(UintNbrType, 8)));
	newNameDclNodeStr("u16", VtypeNameDclNode, (AstNode*)(u16Type = newNbrTypeNode(UintNbrType, 16)));
	newNameDclNodeStr("u32", VtypeNameDclNode, (AstNode*)(u32Type = newNbrTypeNode(UintNbrType, 32)));
	newNameDclNodeStr("u64", VtypeNameDclNode, (AstNode*)(u64Type = newNbrTypeNode(UintNbrType, 64)));
	newNameDclNodeStr("f32", VtypeNameDclNode, (AstNode*)(f32Type = newNbrTypeNode(FloatNbrType, 32)));
	newNameDclNodeStr("f64", VtypeNameDclNode, (AstNode*)(f64Type = newNbrTypeNode(FloatNbrType, 64)));
}

// Create a new primitive number type node
NbrAstNode *newNbrTypeNode(uint16_t typ, char bits) {
	// Start by creating the node for this number type
	NbrAstNode *nbrtypenode;
	newAstNode(nbrtypenode, NbrAstNode, typ);
	if (typ == IntNbrType) {
		nbrtypenode->typenames = itypenames;
	}
	else if (typ == UintNbrType) {
		nbrtypenode->typenames = utypenames;
	}
	else if (typ == FloatNbrType) {
		nbrtypenode->typenames = ftypenames;
	}
	nbrtypenode->traits = nbrtraits;
	nbrtypenode->bits = bits;

	// Create function signature for binary methods using on this type
	FnSigAstNode *binsig = newFnSigNode();
	binsig->rettype = (AstNode*)nbrtypenode;
	Symbol *parm1 = symFind("a", 1);
	inodesAdd(&binsig->parms, parm1, (AstNode *)newNameDclNode(parm1, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));
	Symbol *parm2 = symFind("b", 1);
	inodesAdd(&binsig->parms, parm2, (AstNode *)newNameDclNode(parm2, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));

	// Build method dictionary for the type, which ultimately point to internal op codes
	nbrtypenode->instnames = newInodes(16);
	Symbol *opsym = symFind("+", 1);
	AstNode *xxx;
	inodesAdd(&nbrtypenode->instnames, opsym, xxx = (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(AddOpCode)));
	if (bits == 32 && typ == IntNbrType)
		opsym->node = xxx;
	opsym = symFind("-", 1);
	inodesAdd(&nbrtypenode->instnames, opsym, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(SubOpCode)));

	return nbrtypenode;
}

// Serialize the AST for a numeric literal
void nbrTypePrint(NbrAstNode *node) {
	if (node==i8Type)
		astFprint("i8");
	else if (node == i16Type)
		astFprint("i16");
	else if (node == i32Type)
		astFprint("i32");
	else if (node == i64Type)
		astFprint("i64");
	else if (node == u8Type)
		astFprint("u8");
	else if (node == u16Type)
		astFprint("u16");
	else if (node == u32Type)
		astFprint("u32");
	else if (node == u64Type)
		astFprint("u64");
	else if (node == f32Type)
		astFprint("f32");
	else if (node == f64Type)
		astFprint("f64");
}

// Is a number-typed node
int isNbr(AstNode *node) {
	return (node->asttype == IntNbrType || node->asttype == UintNbrType || node->asttype == FloatNbrType);
}