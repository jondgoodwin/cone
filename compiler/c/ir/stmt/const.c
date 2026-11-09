/** Handling for constant declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new constant declaraction node
ConstDclNode *newConstDclNode(Name *namesym) {
    ConstDclNode *name;
    newNode(name, ConstDclNode, ConstDclTag);
    name->vtype = unknownType;
    name->namesym = namesym;
    name->value = NULL;
    name->owner = NULL;
    return name;
}

// Clone a constant declaration, in two steps like a static (cloneStructNode):
// the shell is made and bound in the instance's namespace first, and the type and
// value are copied after, so a use in any member body maps to the copy. memcpy
// carries the type check marks with everything else, and a clone that kept them
// would be skipped by the guard in inodeTypeCheck, so they are cleared. A module's
// const is never cloned (cloning is entered on a function, struct or macro body);
// a trait's const is refused at parse (ErrorConstPlace).
ConstDclNode *cloneConstDclShell(ConstDclNode *node) {
    ConstDclNode *newnode = memAllocBlk(sizeof(ConstDclNode));
    memcpy(newnode, node, sizeof(ConstDclNode));
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    return newnode;
}

void cloneConstDclFill(CloneState *cstate, ConstDclNode *newnode, ConstDclNode *node) {
    newnode->vtype = cloneNode(cstate, node->vtype);
    newnode->value = cloneNode(cstate, node->value);
    cloneDclSetMap((INode*)node, (INode*)newnode);
}

INode *cloneConstDclNode(CloneState *cstate, ConstDclNode *node) {
    ConstDclNode *newnode = cloneConstDclShell(node);
    cloneConstDclFill(cstate, newnode, node);
    return (INode*)newnode;
}

// Serialize a constant node
void constDclPrint(ConstDclNode *name) {
    inodeFprint("const %s ", &name->namesym->namestr);
    inodePrintNode(name->vtype);
    inodeFprint(" = ");
    if (name->value->tag == BlockTag)
        inodePrintNL();
    inodePrintNode(name->value);
}

// Enable name resolution of local variables
void constDclNameRes(NameResState *pstate, ConstDclNode *name) {
    if (name->vtype)
        inodeNameRes(pstate, &name->vtype);

    // Name resolve value before hooking the constant name (so it cannot point to itself)
    if (name->value)
        inodeNameRes(pstate, &name->value);

}

// Type check constant against its initial value
void constDclTypeCheck(TypeCheckState *pstate, ConstDclNode *name) {
    if (itypeTypeCheck(pstate, &name->vtype) == 0)
        return;

    // A layout fact is the code generator's constant, not a value the front end can hold in a const
    if (sizeofConstRefused(name->value, "a 'const' initializer"))
        return;

    // An initializer need not be specified, but if not, it must have a declared type
    // Verify that declared type and initial value type match
    if (!iexpTypeCheckCoerce(pstate, name->vtype, &name->value))
        iexpMismatchMsg(name->value, name->vtype, ErrorInvType, "Initialization value's type does not match named constant's declared type");
    else if (name->vtype == unknownType)
        name->vtype = ((IExpNode *)name->value)->vtype;
    // Every use of a const is a fresh copy of its literal, so an atomic value
    // in one could never be shared by the operations meant to share it
    if (name->vtype != unknownType && itypeHoldsAtomic(name->vtype))
        errorMsgNode((INode*)name, ErrorAtomicValueConst,
            "The const %s holds an atomic value (its type is %s), and each use of a const is a fresh copy no other use would see change. Use a global.",
            &name->namesym->namestr, itypeName(name->vtype));
    // Constants must be literal: an expression of constants is folded into the
    // one it computes (litFoldConst)
    if (!litFoldConst(&name->value))
        errorMsgNode(name->value, ErrorNotLit, "Named constants must be assigned to a constant value.");
}
