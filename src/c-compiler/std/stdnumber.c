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
#include <string.h>

Nodes *nbrsubtypes;

// Declare built-in number types and their names
void stdNbrInit() {
	nbrsubtypes = newNodes(8);	// Needs 'copy' etc.

	boolType = newNbrTypeNode("Bool", UintNbrType, 1);
	u8Type = newNbrTypeNode("u8", UintNbrType, 8);
	u16Type = newNbrTypeNode("u16", UintNbrType, 16);
	u32Type = newNbrTypeNode("u32", UintNbrType, 32);
	u64Type = newNbrTypeNode("u64", UintNbrType, 64);
	usizeType = newNbrTypeNode("usize", UintNbrType, 0);
	i8Type = newNbrTypeNode("i8", IntNbrType, 8);
	i16Type = newNbrTypeNode("i16", IntNbrType, 16);
	i32Type = newNbrTypeNode("i32", IntNbrType, 32);
	i64Type = newNbrTypeNode("i64", IntNbrType, 64);
	isizeType = newNbrTypeNode("isize", UintNbrType, 0);
	f32Type = newNbrTypeNode("f32", FloatNbrType, 32);
	f64Type = newNbrTypeNode("f64", FloatNbrType, 64);

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
NbrAstNode *newNbrTypeNode(char *name, uint16_t typ, char bits) {
    Name *namesym = nametblFind(name, strlen(name));

	// Start by creating the node for this number type
	NbrAstNode *nbrtypenode;
	newAstNode(nbrtypenode, NbrAstNode, typ);
    nbrtypenode->vtype = NULL;
    nbrtypenode->owner = NULL;
    nbrtypenode->namesym = namesym;
    nbrtypenode->llvmtype = NULL;
    methnodesInit(&nbrtypenode->methfields, 32);
    nbrtypenode->subtypes = nbrsubtypes;
	nbrtypenode->bits = bits;

    namesym->node = (NamedAstNode*)nbrtypenode;

	// Create function signature for unary methods for this type
	FnSigAstNode *unarysig = newFnSigNode();
	unarysig->rettype = (AstNode*)nbrtypenode;
	Name *una1 = nametblFind("a", 1);
	nodesAdd(&unarysig->parms, (AstNode *)newVarDclNode(una1, VarDclTag, (AstNode*)nbrtypenode, immPerm, NULL));

	// Create function signature for binary methods for this type
	FnSigAstNode *binsig = newFnSigNode();
	binsig->rettype = (AstNode*)nbrtypenode;
	Name *parm1 = nametblFind("a", 1);
	nodesAdd(&binsig->parms, (AstNode *)newVarDclNode(parm1, VarDclTag, (AstNode*)nbrtypenode, immPerm, NULL));
	Name *parm2 = nametblFind("b", 1);
	nodesAdd(&binsig->parms, (AstNode *)newVarDclNode(parm2, VarDclTag, (AstNode*)nbrtypenode, immPerm, NULL));

	// Build method dictionary for the type, which ultimately point to intrinsics
	Name *opsym;

	// Arithmetic operators (not applicable to boolean)
	if (bits > 1) {
		opsym = nametblFind("neg", 3);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)unarysig, (AstNode *)newIntrinsicNode(NegIntrinsic)));
		opsym = nametblFind("+", 1);
        methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(AddIntrinsic)));
		opsym = nametblFind("-", 1);
        methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(SubIntrinsic)));
		opsym = nametblFind("*", 1);
        methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(MulIntrinsic)));
		opsym = nametblFind("/", 1);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(DivIntrinsic)));
		opsym = nametblFind("%", 1);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(RemIntrinsic)));
	}

	// Bitwise operators (integer only)
	if (typ != FloatNbrType) {
		opsym = nametblFind("~", 1);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)unarysig, (AstNode *)newIntrinsicNode(NotIntrinsic)));
		opsym = nametblFind("&", 1);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(AndIntrinsic)));
		opsym = nametblFind("|", 1);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(OrIntrinsic)));
		opsym = nametblFind("^", 1);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(XorIntrinsic)));
		if (bits > 1) {
			opsym = nametblFind("shl", 3);
			methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(ShlIntrinsic)));
			opsym = nametblFind("shr", 3);
			methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)binsig, (AstNode *)newIntrinsicNode(ShrIntrinsic)));
		}
	}
	// Floating point functions (intrinsics)
	else {
		opsym = nametblFind("sqrt", 4);
		methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)unarysig, (AstNode *)newIntrinsicNode(SqrtIntrinsic)));
	}

	// Create function signature for comparison methods for this type
	FnSigAstNode *cmpsig = newFnSigNode();
	cmpsig->rettype = bits==1? (AstNode*)nbrtypenode : (AstNode*)boolType;
	nodesAdd(&cmpsig->parms, (AstNode *)newVarDclNode(parm1, VarDclTag, (AstNode*)nbrtypenode, immPerm, NULL));
	nodesAdd(&cmpsig->parms, (AstNode *)newVarDclNode(parm2, VarDclTag, (AstNode*)nbrtypenode, immPerm, NULL));

	// Comparison operators
	opsym = nametblFind("==", 2);
	methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)cmpsig, (AstNode *)newIntrinsicNode(EqIntrinsic)));
	opsym = nametblFind("!=", 2);
	methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)cmpsig, (AstNode *)newIntrinsicNode(NeIntrinsic)));
	opsym = nametblFind("<", 1);
	methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)cmpsig, (AstNode *)newIntrinsicNode(LtIntrinsic)));
	opsym = nametblFind("<=", 2);
	methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)cmpsig, (AstNode *)newIntrinsicNode(LeIntrinsic)));
	opsym = nametblFind(">", 1);
	methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)cmpsig, (AstNode *)newIntrinsicNode(GtIntrinsic)));
	opsym = nametblFind(">=", 2);
	methnodesAddFn(&nbrtypenode->methfields, newFnDclNode(opsym, FlagMethField, (AstNode *)cmpsig, (AstNode *)newIntrinsicNode(GeIntrinsic)));

	return nbrtypenode;
}
