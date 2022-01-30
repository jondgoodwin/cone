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
    return name;
}

// Create a new constant dcl node that is a copy of an existing one
INode *cloneConstDclNode(CloneState *cstate, ConstDclNode *node) {
    ConstDclNode *newnode = memAllocBlk(sizeof(ConstDclNode));
    memcpy(newnode, node, sizeof(ConstDclNode));
    newnode->vtype = cloneNode(cstate, node->vtype);
    newnode->value = cloneNode(cstate, node->value);
    cloneDclSetMap((INode*)node, (INode*)newnode);
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

    // An initializer need not be specified, but if not, it must have a declared type
    // Verify that declared type and initial value type match
    if (!iexpTypeCheckCoerce(pstate, name->vtype, &name->value))
        errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match named constant's declared type");
    else if (name->vtype == unknownType)
        name->vtype = ((IExpNode *)name->value)->vtype;
    // Constants must be literal
    if (!litIsLiteral(name->value))
        errorMsgNode(name->value, ErrorNotLit, "Named constants must be assigned to a constant value.");
}
