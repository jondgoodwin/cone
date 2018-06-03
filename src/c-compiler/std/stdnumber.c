/** Built-in number types and methods
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ast/ast.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ast/nametbl.h"

Nodes *nbrsubtypes;

// Declare built-in number types and their names
void stdNbrInit() {
	nbrsubtypes = newNodes(8);	// Needs 'copy' etc.

	newTypeDclNodeStr("Bool", VtypeNameDclNode, (AstNode*)(boolType = newNbrTypeNode(UintNbrType, 1)));
	newTypeDclNodeStr("u8", VtypeNameDclNode, (AstNode*)(u8Type = newNbrTypeNode(UintNbrType, 8)));
	newTypeDclNodeStr("u16", VtypeNameDclNode, (AstNode*)(u16Type = newNbrTypeNode(UintNbrType, 16)));
	newTypeDclNodeStr("u32", VtypeNameDclNode, (AstNode*)(u32Type = newNbrTypeNode(UintNbrType, 32)));
	newTypeDclNodeStr("u64", VtypeNameDclNode, (AstNode*)(u64Type = newNbrTypeNode(UintNbrType, 64)));
	newTypeDclNodeStr("usize", VtypeNameDclNode, (AstNode*)(usizeType = newNbrTypeNode(UintNbrType, 0)));
	newTypeDclNodeStr("i8", VtypeNameDclNode, (AstNode*)(i8Type = newNbrTypeNode(IntNbrType, 8)));
	newTypeDclNodeStr("i16", VtypeNameDclNode, (AstNode*)(i16Type = newNbrTypeNode(IntNbrType, 16)));
	newTypeDclNodeStr("i32", VtypeNameDclNode, (AstNode*)(i32Type = newNbrTypeNode(IntNbrType, 32)));
	newTypeDclNodeStr("i64", VtypeNameDclNode, (AstNode*)(i64Type = newNbrTypeNode(IntNbrType, 64)));
	newTypeDclNodeStr("isize", VtypeNameDclNode, (AstNode*)(isizeType = newNbrTypeNode(UintNbrType, 0)));
	newTypeDclNodeStr("f32", VtypeNameDclNode, (AstNode*)(f32Type = newNbrTypeNode(FloatNbrType, 32)));
	newTypeDclNodeStr("f64", VtypeNameDclNode, (AstNode*)(f64Type = newNbrTypeNode(FloatNbrType, 64)));

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
	Name *una1 = nameFind("a", 1);
	inodesAdd(&unarysig->parms, una1, (AstNode *)newNameDclNode(una1, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));

	// Create function signature for binary methods for this type
	FnSigAstNode *binsig = newFnSigNode();
	binsig->rettype = (AstNode*)nbrtypenode;
	Name *parm1 = nameFind("a", 1);
	inodesAdd(&binsig->parms, parm1, (AstNode *)newNameDclNode(parm1, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));
	Name *parm2 = nameFind("b", 1);
	inodesAdd(&binsig->parms, parm2, (AstNode *)newNameDclNode(parm2, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));

	// Build method dictionary for the type, which ultimately point to intrinsics
	nbrtypenode->methods = newNodes(16);
	Name *opsym;

	// Arithmetic operators (not applicable to boolean)
	if (bits > 1) {
		opsym = nameFind("neg", 3);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)unarysig, immPerm, (AstNode *)newIntrinsicNode(NegIntrinsic)));
		opsym = nameFind("+", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(AddIntrinsic)));
		opsym = nameFind("-", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(SubIntrinsic)));
		opsym = nameFind("*", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(MulIntrinsic)));
		opsym = nameFind("/", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(DivIntrinsic)));
		opsym = nameFind("%", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(RemIntrinsic)));
	}

	// Bitwise operators (integer only)
	if (typ != FloatNbrType) {
		opsym = nameFind("~", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)unarysig, immPerm, (AstNode *)newIntrinsicNode(NotIntrinsic)));
		opsym = nameFind("&", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(AndIntrinsic)));
		opsym = nameFind("|", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(OrIntrinsic)));
		opsym = nameFind("^", 1);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(XorIntrinsic)));
		if (bits > 1) {
			opsym = nameFind("shl", 3);
			nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(ShlIntrinsic)));
			opsym = nameFind("shr", 3);
			nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)binsig, immPerm, (AstNode *)newIntrinsicNode(ShrIntrinsic)));
		}
	}
	// Floating point functions (intrinsics)
	else {
		opsym = nameFind("sqrt", 4);
		nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)unarysig, immPerm, (AstNode *)newIntrinsicNode(SqrtIntrinsic)));
	}

	// Create function signature for comparison methods for this type
	FnSigAstNode *cmpsig = newFnSigNode();
	cmpsig->rettype = bits==1? (AstNode*)nbrtypenode : (AstNode*)boolType;
	inodesAdd(&cmpsig->parms, parm1, (AstNode *)newNameDclNode(parm1, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));
	inodesAdd(&cmpsig->parms, parm2, (AstNode *)newNameDclNode(parm2, VarNameDclNode, (AstNode*)nbrtypenode, immPerm, NULL));

	// Comparison operators
	opsym = nameFind("==", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newIntrinsicNode(EqIntrinsic)));
	opsym = nameFind("!=", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newIntrinsicNode(NeIntrinsic)));
	opsym = nameFind("<", 1);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newIntrinsicNode(LtIntrinsic)));
	opsym = nameFind("<=", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newIntrinsicNode(LeIntrinsic)));
	opsym = nameFind(">", 1);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newIntrinsicNode(GtIntrinsic)));
	opsym = nameFind(">=", 2);
	nodesAdd(&nbrtypenode->methods, (AstNode *)newNameDclNode(opsym, VarNameDclNode, (AstNode *)cmpsig, immPerm, (AstNode *)newIntrinsicNode(GeIntrinsic)));

	return nbrtypenode;
}
