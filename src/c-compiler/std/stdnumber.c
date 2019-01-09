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
	nodesAdd(&unarysig->parms, (INode *)newVarDclFull(una1, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));

    // Create function signature for unary ref methods for this type
    FnSigNode *mutrefsig = newFnSigNode();
    mutrefsig->rettype = (INode*)nbrtypenode;
    RefNode *mutref = newRefNode();
    mutref->pvtype = (INode*)nbrtypenode;
    mutref->alloc = voidType;
    mutref->perm = newPermUseNode((INamedNode*)mutPerm);
    nodesAdd(&mutrefsig->parms, (INode *)newVarDclFull(una1, VarDclTag, (INode*)mutref, newPermUseNode((INamedNode*)immPerm), NULL));

    // Create function signature for binary methods for this type
	FnSigNode *binsig = newFnSigNode();
	binsig->rettype = (INode*)nbrtypenode;
	Name *parm1 = nametblFind("a", 1);
	nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm1, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));
	Name *parm2 = nametblFind("b", 1);
	nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));

	// Build method dictionary for the type, which ultimately point to intrinsics
	Name *opsym;

	// Arithmetic operators (not applicable to boolean)
	if (bits > 1) {
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(minusName, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(NegIntrinsic)));
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(incrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrIntrinsic)));
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(decrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrIntrinsic)));
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(incrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrPostIntrinsic)));
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(decrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrPostIntrinsic)));
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(plusName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(AddIntrinsic)));
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(minusName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(SubIntrinsic)));
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(multName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(MulIntrinsic)));
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(divName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(DivIntrinsic)));
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(remName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(RemIntrinsic)));
	}

	// Bitwise operators (integer only)
	if (typ != FloatNbrTag) {
		opsym = nametblFind("~", 1);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(NotIntrinsic)));
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(andName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(AndIntrinsic)));
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(orName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(OrIntrinsic)));
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(xorName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(XorIntrinsic)));
		if (bits > 1) {
			imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(shlName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(ShlIntrinsic)));
			imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(shrName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(ShrIntrinsic)));
		}
	}
	// Floating point functions (intrinsics)
	else {
		opsym = nametblFind("sqrt", 4);
		imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(SqrtIntrinsic)));
        opsym = nametblFind("sin", 3);
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(SinIntrinsic)));
        opsym = nametblFind("cos", 3);
        imethnodesAddFn(&nbrtypenode->methprops, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(CosIntrinsic)));
    }

	// Create function signature for comparison methods for this type
	FnSigNode *cmpsig = newFnSigNode();
	cmpsig->rettype = bits==1? (INode*)nbrtypenode : (INode*)boolType;
	nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm1, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));
	nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)nbrtypenode, newPermUseNode((INamedNode*)immPerm), NULL));

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

// Create a ptr type for holding valid pointer methods
IMethodNode *newPtrTypeMethods() {

    // Create the node for this pointer type
    IMethodNode *ptrtypenode;
    newNode(ptrtypenode, IMethodNode, PtrTag);
    ptrtypenode->vtype = NULL;
    ptrtypenode->owner = NULL;
    ptrtypenode->namesym = NULL;
    ptrtypenode->llvmtype = NULL;
    imethnodesInit(&ptrtypenode->methprops, 16);
    ptrtypenode->subtypes = nbrsubtypes;

    PtrNode *voidptr = newPtrNode();
    voidptr->pvtype = voidType;

    // Create function signature for unary methods for this type
    FnSigNode *unarysig = newFnSigNode();
    unarysig->rettype = (INode*)voidptr;
    Name *self = nametblFind("self", 4);
    nodesAdd(&unarysig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidptr, newPermUseNode((INamedNode*)immPerm), NULL));

    // Create function signature for binary methods for this type
    FnSigNode *binsig = newFnSigNode();
    binsig->rettype = (INode*)voidptr;
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidptr, newPermUseNode((INamedNode*)immPerm), NULL));
    Name *parm2 = nametblFind("b", 1);
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidptr, newPermUseNode((INamedNode*)immPerm), NULL));

    // Create function signature for comparison methods for this type
    FnSigNode *cmpsig = newFnSigNode();
    cmpsig->rettype = (INode*)boolType;
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidptr, newPermUseNode((INamedNode*)immPerm), NULL));
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidptr, newPermUseNode((INamedNode*)immPerm), NULL));

    // Comparison operators
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(eqName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(EqIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(neName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(NeIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(ltName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LtIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(leName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LeIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(gtName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GtIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(geName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GeIntrinsic)));

    // Create function signature for unary ref methods for ++, --
    FnSigNode *mutrefsig = newFnSigNode();
    mutrefsig->rettype = (INode*)voidptr;
    RefNode *mutref = newRefNode();
    mutref->pvtype = (INode*)voidptr;
    mutref->alloc = voidType;
    mutref->perm = newPermUseNode((INamedNode*)mutPerm);
    Name *una1 = nametblFind("a", 1);
    nodesAdd(&mutrefsig->parms, (INode *)newVarDclFull(una1, VarDclTag, (INode*)mutref, newPermUseNode((INamedNode*)immPerm), NULL));

    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(incrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(decrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(incrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrPostIntrinsic)));
    imethnodesAddFn(&ptrtypenode->methprops, newFnDclNode(decrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrPostIntrinsic)));

    return ptrtypenode;
}

// Create a ptr type for holding valid pointer methods
IMethodNode *newRefTypeMethods() {

    // Create the node for this pointer type
    IMethodNode *reftypenode;
    newNode(reftypenode, IMethodNode, RefTag);
    reftypenode->vtype = NULL;
    reftypenode->owner = NULL;
    reftypenode->namesym = NULL;
    reftypenode->llvmtype = NULL;
    imethnodesInit(&reftypenode->methprops, 8);
    reftypenode->subtypes = nbrsubtypes;

    RefNode *voidref = newRefNode();
    voidref->alloc = voidType;
    voidref->perm = (INode*)constPerm;
    voidref->pvtype = voidType;

    // Create function signature for unary methods for this type
    FnSigNode *unarysig = newFnSigNode();
    unarysig->rettype = (INode*)voidref;
    Name *self = nametblFind("self", 4);
    nodesAdd(&unarysig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode((INamedNode*)immPerm), NULL));

    // Create function signature for binary methods for this type
    FnSigNode *binsig = newFnSigNode();
    binsig->rettype = (INode*)voidref;
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode((INamedNode*)immPerm), NULL));
    Name *parm2 = nametblFind("b", 1);
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidref, newPermUseNode((INamedNode*)immPerm), NULL));

    // Create function signature for comparison methods for this type
    FnSigNode *cmpsig = newFnSigNode();
    cmpsig->rettype = (INode*)boolType;
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode((INamedNode*)immPerm), NULL));
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidref, newPermUseNode((INamedNode*)immPerm), NULL));

    // Comparison operators
    imethnodesAddFn(&reftypenode->methprops, newFnDclNode(eqName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(EqIntrinsic)));
    imethnodesAddFn(&reftypenode->methprops, newFnDclNode(neName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(NeIntrinsic)));
    imethnodesAddFn(&reftypenode->methprops, newFnDclNode(ltName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LtIntrinsic)));
    imethnodesAddFn(&reftypenode->methprops, newFnDclNode(leName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LeIntrinsic)));
    imethnodesAddFn(&reftypenode->methprops, newFnDclNode(gtName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GtIntrinsic)));
    imethnodesAddFn(&reftypenode->methprops, newFnDclNode(geName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GeIntrinsic)));

    return reftypenode;
}

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

    ptrType = newPtrTypeMethods();
    refType = newRefTypeMethods();
}