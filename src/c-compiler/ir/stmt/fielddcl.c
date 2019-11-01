/** Handling for field declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new name declaraction node
FieldDclNode *newFieldDclNode(Name *namesym, INode *perm) {
    FieldDclNode *fldnode;
    newNode(fldnode, FieldDclNode, FieldDclTag);
    fldnode->vtype = voidType;
    fldnode->namesym = namesym;
    fldnode->perm = perm;
    fldnode->value = NULL;
    fldnode->index = 0;
    return fldnode;
}

// Serialize a field declaration node
void fieldDclPrint(FieldDclNode *name) {
    inodePrintNode((INode*)name->perm);
    inodeFprint(" %s ", &name->namesym->namestr);
    inodePrintNode(name->vtype);
    if (name->value) {
        inodeFprint(" = ");
        if (name->value->tag == BlockTag)
            inodePrintNL();
        inodePrintNode(name->value);
    }
}

// Enable name resolution of field declarations
void fieldDclNameRes(NameResState *pstate, FieldDclNode *name) {
    inodeNameRes(pstate, (INode**)&name->perm);
    inodeNameRes(pstate, &name->vtype);

    if (name->value)
        inodeNameRes(pstate, &name->value);
}

// Type check field declaration against its initial value
void fieldDclTypeCheck(TypeCheckState *pstate, FieldDclNode *name) {
    inodeTypeCheck(pstate, (INode**)&name->perm);
    inodeTypeCheck(pstate, &name->vtype);

    // An initializer need not be specified, but if not, it must have a declared type
    if (!name->value) {
        if (name->vtype == voidType)
            errorMsgNode((INode*)name, ErrorNoType, "Declared field must specify a type or value");
        return;
    }
    // Type check the initialization value
    else {
        inodeTypeCheck(pstate, &name->value);
        // Fields require literal default values
        if (!litIsLiteral(name->value))
            errorMsgNode(name->value, ErrorNotLit, "Field default must be a literal value.");
        // Infer the var's vtype from its value, if not provided
        if (name->vtype == voidType)
            name->vtype = ((IExpNode *)name->value)->vtype;
        // Otherwise, verify that declared type and initial value type matches
        else if (!iexpCoerces(name->vtype, &name->value))
            errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
    }

    // Fields cannot hold a void or opaque struct value
    if (!itypeIsConcrete(name->vtype))
        errorMsgNode(name->vtype, ErrorInvType, "Field's type must be concrete and instantiable.");
}
