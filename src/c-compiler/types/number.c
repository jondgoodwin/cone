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

Nodes *nbrsubtypes;

// Declare built-in number types and their names
void nbrDclNames() {
	nbrsubtypes = newNodes(8);	// Needs 'copy' etc.

	newNameDclNodeStr("Bool", VtypeNameDclNode, (AstNode*)(boolType = newNbrTypeNode(UintNbrType, 1)));
	newNameDclNodeStr("u8", VtypeNameDclNode, (AstNode*)(u8Type = newNbrTypeNode(UintNbrType, 8)));
	newNameDclNodeStr("u16", VtypeNameDclNode, (AstNode*)(u16Type = newNbrTypeNode(UintNbrType, 16)));
	newNameDclNodeStr("u32", VtypeNameDclNode, (AstNode*)(u32Type = newNbrTypeNode(UintNbrType, 32)));
	newNameDclNodeStr("u64", VtypeNameDclNode, (AstNode*)(u64Type = newNbrTypeNode(UintNbrType, 64)));
	newNameDclNodeStr("i8", VtypeNameDclNode, (AstNode*)(i8Type = newNbrTypeNode(IntNbrType, 8)));
	newNameDclNodeStr("i16", VtypeNameDclNode, (AstNode*)(i16Type = newNbrTypeNode(IntNbrType, 16)));
	newNameDclNodeStr("i32", VtypeNameDclNode, (AstNode*)(i32Type = newNbrTypeNode(IntNbrType, 32)));
	newNameDclNodeStr("i64", VtypeNameDclNode, (AstNode*)(i64Type = newNbrTypeNode(IntNbrType, 64)));
	newNameDclNodeStr("f32", VtypeNameDclNode, (AstNode*)(f32Type = newNbrTypeNode(FloatNbrType, 32)));
	newNameDclNodeStr("f64", VtypeNameDclNode, (AstNode*)(f64Type = newNbrTypeNode(FloatNbrType, 64)));

	// Reference to a literal string
	ArrayAstNode *strArr = newArrayNode();
	strArr->size = 0;
	strArr->elemtype = (AstNode*)u8Type;
	strType = newPtrTypeNode();
	strType->pvtype = (AstNode*)/*strArr*/u8Type;
	strType->perm = immPerm;
	strType->alloc = NULL;
	strType->scope = 0;
}

// Create a new primitive number type node
NbrAstNode *newNbrTypeNode(uint16_t typ, char bits) {
	// Start by creating the node for this number type
	NbrAstNode *nbrtypenode;
	newAstNode(nbrtypenode, NbrAstNode, typ);
	nbrtypenode->subtypes = nbrsubtypes;
	nbrtypenode->bits = bits;

	// Create function signature for unary methods for this type
	FnSigAstNode *unarysig = newFnSigNode();
	unarysig->rettype = (AstNode*)nbrtypenode;
	Symbol *una1 = symFind("a", 1);
	inodesAdd(&unarysig->parms, una1, (AstNode *)newNameDclNode(una1, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));

	// Create function signature for binary methods for this type
	FnSigAstNode *binsig = newFnSigNode();
	binsig->rettype = (AstNode*)nbrtypenode;
	Symbol *parm1 = symFind("a", 1);
	inodesAdd(&binsig->parms, parm1, (AstNode *)newNameDclNode(parm1, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));
	Symbol *parm2 = symFind("b", 1);
	inodesAdd(&binsig->parms, parm2, (AstNode *)newNameDclNode(parm2, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));

	// Build method dictionary for the type, which ultimately point to internal op codes
	nbrtypenode->methods = newNodes(16);
	Symbol *opsym;

	// Arithmetic operators (not applicable to boolean)
	if (bits > 1) {
		opsym = symFind("neg", 3);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)unarysig, immPerm, (AstNode *)newOpCodeNode(NegOpCode)));
		opsym = symFind("+", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(AddOpCode)));
		opsym = symFind("-", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(SubOpCode)));
		opsym = symFind("*", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(MulOpCode)));
		opsym = symFind("/", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(DivOpCode)));
		opsym = symFind("%", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(RemOpCode)));
	}

	// Bitwise operators (integer only)
	if (typ != FloatNbrType) {
		opsym = symFind("~", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)unarysig, immPerm, (AstNode *)newOpCodeNode(NotOpCode)));
		opsym = symFind("&", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(AndOpCode)));
		opsym = symFind("|", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(OrOpCode)));
		opsym = symFind("^", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(XorOpCode)));
		if (bits > 1) {
			opsym = symFind("shl", 3);
			nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(ShlOpCode)));
			opsym = symFind("shr", 3);
			nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newOpCodeNode(ShrOpCode)));
		}
	}
	// Floating point functions (intrinsics)
	else {
		opsym = symFind("sqrt", 4);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)unarysig, immPerm, (AstNode *)newOpCodeNode(SqrtOpCode)));
	}

	// Create function signature for comparison methods for this type
	FnSigAstNode *cmpsig = newFnSigNode();
	cmpsig->rettype = bits==1? (AstNode*)nbrtypenode : (AstNode*)boolType;
	inodesAdd(&cmpsig->parms, parm1, (AstNode *)newNameDclNode(parm1, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));
	inodesAdd(&cmpsig->parms, parm2, (AstNode *)newNameDclNode(parm2, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));

	// Comparison operators
	opsym = symFind("==", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newOpCodeNode(EqOpCode)));
	opsym = symFind("!=", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newOpCodeNode(NeOpCode)));
	opsym = symFind("<", 1);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newOpCodeNode(LtOpCode)));
	opsym = symFind("<=", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newOpCodeNode(LeOpCode)));
	opsym = symFind(">", 1);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newOpCodeNode(GtOpCode)));
	opsym = symFind(">=", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newOpCodeNode(GeOpCode)));

	return nbrtypenode;
}

// Serialize the AST for a numeric literal
void nbrTypePrint(NbrAstNode *node) {
	if (node == i8Type)
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
	else if (node == boolType)
		astFprint("Bool");
}

// Is a number-typed node
int isNbr(AstNode *node) {
	return (node->asttype == IntNbrType || node->asttype == UintNbrType || node->asttype == FloatNbrType);
}