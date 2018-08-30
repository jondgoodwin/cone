/** Built-in number types and methods
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../shared/memory.h"
#include "../parser/lexer.h"
#include "../ir/nametbl.h"
#include <string.h>

Nodes *nbrsubtypes;

// Declare built-in number types and their names
void stdNbrInit() {
	nbrsubtypes = newNodes(8);	// Needs 'copy' etc.

	boolType = newNbrTypeNode("Bool", UintNbrTag, 1);
	u8Type = newNbrTypeNode("u8", UintNbrTag, 8);
	u16Type = newNbrTypeNode("u16", UintNbrTag, 16);
	u32Type = newNbrTypeNode("u32", UintNbrTag, 32);
	u64Type = newNbrTypeNode("u64", UintNbrTag, 64);
	usizeType = newNbrTypeNode("usize", UintNbrTag, 0);
	i8Type = newNbrTypeNode("i8", IntNbrTag, 8);
	i16Type = newNbrTypeNode("i16", IntNbrTag, 16);
	i32Type = newNbrTypeNode("i32", IntNbrTag, 32);
	i64Type = newNbrTypeNode("i64", IntNbrTag, 64);
	isizeType = newNbrTypeNode("isize", UintNbrTag, 0);
	f32Type = newNbrTypeNode("f32", FloatNbrTag, 32);
	f64Type = newNbrTypeNode("f64", FloatNbrTag, 64);

	// Reference to a literal string
	ArrayNode *strArr = newArrayNode();
	strArr->size = 0;
	strArr->elemtype = (INode*)u8Type;
	strType = newPtrNode();
	strType->pvtype = (INode*)/*strArr*/u8Type;
}

// Create a new primitive number type node
NbrNode *newNbrTypeNode(char *name, uint16_t typ, char bits) {
    Name *namesym = nametblFind(name, strlen(name));

	// Start by creating the node for this number type
	NbrNode *nbrtypenode;
	newNode(nbrtypenode, NbrNode, typ);
    nbrtypenode->vtype = NULL;
    nbrtypenode->owner = NULL;
    nbrtypenode->namesym = namesym;
    nbrtypenode->llvmtype = NULL;
    imethnodesInit(&nbrtypenode->methprops, 32);
    nbrtypenode->subtypes = nbrsubtypes;
	nbrtypenode->bits = bits;

    namesym->node = (INamedNode*)nbrtypenode;

	// Create function signature for unary methods for this type
	FnSigNode *unarysig = newFnSigNode();
	unarysig->rettype = (INode*)nbrtypenode;
	Name *una1 = nametblFind("a", 1);
	nodesAdd(&unarysig->parms, (INode *)newVarDclNode(una1, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));

	// Create function signature for binary methods for this type
	FnSigNode *binsig = newFnSigNode();
	binsig->rettype = (INode*)nbrtypenode;
	Name *parm1 = nametblFind("a", 1);
	nodesAdd(&binsig->parms, (INode *)newVarDclNode(parm1, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));
	Name *parm2 = nametblFind("b", 1);
	nodesAdd(&binsig->parms, (INode *)newVarDclNode(parm2, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));

	// Build method dictionary for the type, which ultimately point to intrinsics
	Name *opsym;

	// Arithmetic operators (not applicable to boolean)
	if (bits > 1) {
		opsym = nametblFind("neg", 3);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(NegIntrinsic)));
		opsym = nametblFind("+", 1);
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(AddIntrinsic)));
		opsym = nametblFind("-", 1);
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(SubIntrinsic)));
		opsym = nametblFind("*", 1);
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(MulIntrinsic)));
		opsym = nametblFind("/", 1);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(DivIntrinsic)));
		opsym = nametblFind("%", 1);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(RemIntrinsic)));
	}

	// Bitwise operators (integer only)
	if (typ != FloatNbrTag) {
		opsym = nametblFind("~", 1);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(NotIntrinsic)));
		opsym = nametblFind("&", 1);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(AndIntrinsic)));
		opsym = nametblFind("|", 1);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(OrIntrinsic)));
		opsym = nametblFind("^", 1);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(XorIntrinsic)));
		if (bits > 1) {
			opsym = nametblFind("shl", 3);
			imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(ShlIntrinsic)));
			opsym = nametblFind("shr", 3);
			imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(ShrIntrinsic)));
		}
	}
	// Floating point functions (intrinsics)
	else {
		opsym = nametblFind("sqrt", 4);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(SqrtIntrinsic)));
	}

	// Create function signature for comparison methods for this type
	FnSigNode *cmpsig = newFnSigNode();
	cmpsig->rettype = bits==1? (INode*)nbrtypenode : (INode*)boolType;
	nodesAdd(&cmpsig->parms, (INode *)newVarDclNode(parm1, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));
	nodesAdd(&cmpsig->parms, (INode *)newVarDclNode(parm2, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));

	// Comparison operators
	opsym = nametblFind("==", 2);
	imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(EqIntrinsic)));
	opsym = nametblFind("!=", 2);
	imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(NeIntrinsic)));
	opsym = nametblFind("<", 1);
	imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LtIntrinsic)));
	opsym = nametblFind("<=", 2);
	imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LeIntrinsic)));
	opsym = nametblFind(">", 1);
	imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GtIntrinsic)));
	opsym = nametblFind(">=", 2);
	imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GeIntrinsic)));

	return nbrtypenode;
}
