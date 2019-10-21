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

// Create a new primitive number type node
NbrNode *newNbrTypeNode(char *name, uint16_t typ, char bits) {
    Name *namesym = nametblFind(name, strlen(name));

    // Start by creating the node for this number type
    NbrNode *nbrtypenode;
    newNode(nbrtypenode, NbrNode, typ);
    nbrtypenode->vtype = NULL;
    nbrtypenode->namesym = namesym;
    nbrtypenode->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)nbrtypenode, 32);
    nbrtypenode->bits = bits;

    namesym->node = (INode*)nbrtypenode;

    // Create function signature for unary methods for this type
    FnSigNode *unarysig = newFnSigNode();
    unarysig->rettype = (INode*)nbrtypenode;
    Name *una1 = nametblFind("a", 1);
    nodesAdd(&unarysig->parms, (INode *)newVarDclFull(una1, VarDclTag, (INode*)nbrtypenode, newPermUseNode(immPerm), NULL));

    // Create function signature for unary ref methods for this type
    FnSigNode *mutrefsig = newFnSigNode();
    mutrefsig->rettype = (INode*)nbrtypenode;
    RefNode *mutref = newRefNode();
    mutref->pvtype = (INode*)nbrtypenode;
    mutref->alloc = voidType;
    mutref->perm = newPermUseNode(mutPerm);
    nodesAdd(&mutrefsig->parms, (INode *)newVarDclFull(una1, VarDclTag, (INode*)mutref, newPermUseNode(immPerm), NULL));

    // Create function signature for binary methods for this type
    FnSigNode *binsig = newFnSigNode();
    binsig->rettype = (INode*)nbrtypenode;
    Name *parm1 = nametblFind("a", 1);
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm1, VarDclTag, (INode*)nbrtypenode, newPermUseNode(immPerm), NULL));
    Name *parm2 = nametblFind("b", 1);
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)nbrtypenode, newPermUseNode(immPerm), NULL));

    // Build method dictionary for the type, which ultimately point to intrinsics
    Name *opsym;

    // Arithmetic operators (not applicable to boolean)
    if (bits > 1) {
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(minusName, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(NegIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(incrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(decrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(incrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrPostIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(decrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrPostIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(plusName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(AddIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(minusName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(SubIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(multName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(MulIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(divName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(DivIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(remName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(RemIntrinsic)));
    }

    // Bitwise operators (integer only)
    if (typ != FloatNbrTag) {
        opsym = nametblFind("~", 1);
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(NotIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(andName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(AndIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(orName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(OrIntrinsic)));
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(xorName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(XorIntrinsic)));
        if (bits > 1) {
            iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(shlName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(ShlIntrinsic)));
            iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(shrName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(ShrIntrinsic)));
        }
    }
    // Floating point functions (intrinsics)
    else {
        opsym = nametblFind("sqrt", 4);
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(SqrtIntrinsic)));
        opsym = nametblFind("sin", 3);
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(SinIntrinsic)));
        opsym = nametblFind("cos", 3);
        iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)unarysig, (INode *)newIntrinsicNode(CosIntrinsic)));
    }

    // Create function signature for comparison methods for this type
    FnSigNode *cmpsig = newFnSigNode();
    cmpsig->rettype = bits==1? (INode*)nbrtypenode : (INode*)boolType;
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm1, VarDclTag, (INode*)nbrtypenode, newPermUseNode(immPerm), NULL));
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)nbrtypenode, newPermUseNode(immPerm), NULL));

    // Comparison operators
    opsym = nametblFind("==", 2);
    iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(EqIntrinsic)));
    opsym = nametblFind("!=", 2);
    iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(NeIntrinsic)));
    opsym = nametblFind("<", 1);
    iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LtIntrinsic)));
    opsym = nametblFind("<=", 2);
    iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LeIntrinsic)));
    opsym = nametblFind(">", 1);
    iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GtIntrinsic)));
    opsym = nametblFind(">=", 2);
    iNsTypeAddFn((INsTypeNode*)nbrtypenode, newFnDclNode(opsym, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GeIntrinsic)));

    return nbrtypenode;
}

// Create a generic ptr type for holding valid pointer methods
INsTypeNode *newPtrTypeMethods() {

    // Create the node for this pointer type
    INsTypeNode *ptrtypenode;
    newNode(ptrtypenode, INsTypeNode, PtrTag);
    ptrtypenode->vtype = NULL;
    ptrtypenode->namesym = NULL;
    ptrtypenode->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)ptrtypenode, 16);

    PtrNode *voidptr = newPtrNode();
    voidptr->pvtype = voidType;

    // Create function signature for comparison methods
    FnSigNode *cmpsig = newFnSigNode();
    cmpsig->rettype = (INode*)boolType;
    Name *self = nametblFind("self", 4);
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidptr, newPermUseNode(immPerm), NULL));
    Name *parm2 = nametblFind("b", 1);
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidptr, newPermUseNode(immPerm), NULL));

    // Comparison operators
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(eqName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(EqIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(neName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(NeIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(ltName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LtIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(leName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LeIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(gtName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GtIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(geName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GeIntrinsic)));

    // Create function signature for unary ref methods for ++, --
    FnSigNode *mutrefsig = newFnSigNode();
    mutrefsig->rettype = (INode*)voidptr;
    RefNode *mutref = newRefNode();
    mutref->pvtype = (INode*)voidptr;
    mutref->alloc = voidType;
    mutref->perm = newPermUseNode(mutPerm);
    nodesAdd(&mutrefsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)mutref, newPermUseNode(immPerm), NULL));

    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(incrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(decrName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(incrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(IncrPostIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(decrPostName, FlagMethProp, (INode *)mutrefsig, (INode *)newIntrinsicNode(DecrPostIntrinsic)));

    // Create function signature for + - binary methods
    FnSigNode *binsig = newFnSigNode();
    binsig->rettype = (INode*)voidptr;
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidptr, newPermUseNode(immPerm), NULL));
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)usizeType, newPermUseNode(immPerm), NULL));

    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(plusName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(AddIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(minusName, FlagMethProp, (INode *)binsig, (INode *)newIntrinsicNode(SubIntrinsic)));

    // Create function signature for difference between two pointers
    FnSigNode *diffsig = newFnSigNode();
    diffsig->rettype = (INode*)usizeType;
    nodesAdd(&diffsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidptr, newPermUseNode(immPerm), NULL));
    nodesAdd(&diffsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidptr, newPermUseNode(immPerm), NULL));

    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(minusName, FlagMethProp, (INode *)diffsig, (INode *)newIntrinsicNode(DiffIntrinsic)));

    // Create function signature for += and -= methods
    FnSigNode *bineqsig = newFnSigNode();
    bineqsig->rettype = (INode*)voidptr;
    nodesAdd(&bineqsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)mutref, newPermUseNode(immPerm), NULL));
    nodesAdd(&bineqsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)usizeType, newPermUseNode(immPerm), NULL));

    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(plusEqName, FlagMethProp, (INode *)bineqsig, (INode *)newIntrinsicNode(AddEqIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)ptrtypenode, newFnDclNode(minusEqName, FlagMethProp, (INode *)bineqsig, (INode *)newIntrinsicNode(SubEqIntrinsic)));

    return ptrtypenode;
}

// Create a generic reference type for holding valid reference methods
INsTypeNode *newRefTypeMethods() {

    // Create the node for this reference type
    INsTypeNode *reftypenode;
    newNode(reftypenode, INsTypeNode, RefTag);
    reftypenode->vtype = NULL;
    reftypenode->namesym = NULL;
    reftypenode->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)reftypenode, 8);

    RefNode *voidref = newRefNode();
    voidref->alloc = voidType;
    voidref->perm = (INode*)constPerm;
    voidref->pvtype = voidType;

    // Create function signature for unary methods for this type
    FnSigNode *unarysig = newFnSigNode();
    unarysig->rettype = (INode*)voidref;
    Name *self = nametblFind("self", 4);
    nodesAdd(&unarysig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));

    // Create function signature for binary methods for this type
    FnSigNode *binsig = newFnSigNode();
    binsig->rettype = (INode*)voidref;
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));
    Name *parm2 = nametblFind("b", 1);
    nodesAdd(&binsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));

    // Create function signature for comparison methods for this type
    FnSigNode *cmpsig = newFnSigNode();
    cmpsig->rettype = (INode*)boolType;
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));

    // Comparison operators
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(eqName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(EqIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(neName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(NeIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(ltName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LtIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(leName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(LeIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(gtName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GtIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(geName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(GeIntrinsic)));

    return reftypenode;
}

// Create a generic array reference type for holding valid array reference methods
INsTypeNode *newArrayRefTypeMethods() {

    // Create the node for this array reference type
    INsTypeNode *reftypenode;
    newNode(reftypenode, INsTypeNode, ArrayRefTag);
    reftypenode->vtype = NULL;
    reftypenode->namesym = NULL;
    reftypenode->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)reftypenode, 8);

    RefNode *voidref = newRefNode();
    voidref->tag = ArrayRefTag;
    voidref->alloc = voidType;
    voidref->perm = (INode*)constPerm;
    voidref->pvtype = voidType;

    // '.count' operator
    FnSigNode *countsig = newFnSigNode();
    countsig->rettype = (INode*)usizeType;
    Name *self = nametblFind("self", 4);
    nodesAdd(&countsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(nametblFind("len", 3), FlagMethProp, (INode *)countsig, (INode *)newIntrinsicNode(CountIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(nametblFind("maxlen", 6), FlagMethProp, (INode *)countsig, (INode *)newIntrinsicNode(CountIntrinsic)));

    // Create function signature for comparison methods for this type
    Name *parm2 = nametblFind("b", 1);
    FnSigNode *cmpsig = newFnSigNode();
    cmpsig->rettype = (INode*)boolType;
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(self, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));
    nodesAdd(&cmpsig->parms, (INode *)newVarDclFull(parm2, VarDclTag, (INode*)voidref, newPermUseNode(immPerm), NULL));

    // Comparison operators
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(eqName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(EqIntrinsic)));
    iNsTypeAddFn((INsTypeNode*)reftypenode, newFnDclNode(neName, FlagMethProp, (INode *)cmpsig, (INode *)newIntrinsicNode(NeIntrinsic)));

    return reftypenode;
}

// Declare built-in number types and their names
void stdNbrInit(int ptrsize) {
    boolType = newNbrTypeNode("Bool", UintNbrTag, 1);
    u8Type = newNbrTypeNode("u8", UintNbrTag, 8);
    u16Type = newNbrTypeNode("u16", UintNbrTag, 16);
    u32Type = newNbrTypeNode("u32", UintNbrTag, 32);
    u64Type = newNbrTypeNode("u64", UintNbrTag, 64);
    usizeType = newNbrTypeNode("usize", UintNbrTag, ptrsize);
    i8Type = newNbrTypeNode("i8", IntNbrTag, 8);
    i16Type = newNbrTypeNode("i16", IntNbrTag, 16);
    i32Type = newNbrTypeNode("i32", IntNbrTag, 32);
    i64Type = newNbrTypeNode("i64", IntNbrTag, 64);
    isizeType = newNbrTypeNode("isize", IntNbrTag, ptrsize);
    f32Type = newNbrTypeNode("f32", FloatNbrTag, 32);
    f64Type = newNbrTypeNode("f64", FloatNbrTag, 64);

    ptrType = newPtrTypeMethods();
    refType = newRefTypeMethods();
    arrayRefType = newArrayRefTypeMethods();
}