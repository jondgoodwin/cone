/** Handling for structs
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new struct type whose info will be filled in afterwards
StructNode *newStructNode(Name *namesym) {
    StructNode *snode;
    newNode(snode, StructNode, StructTag);
    snode->namesym = namesym;
    snode->llvmtype = NULL;
    snode->subtypes = newNodes(0);
    iNsTypeInit((INsTypeNode*)snode, 8);
    nodelistInit(&snode->fields, 8);
    snode->mod = NULL;
    snode->basetrait = NULL;
    return snode;
}

// Add a field
void structAddField(StructNode *type, FieldDclNode *fnode) {
    INode *foundnode = namespaceAdd(&type->namespace, fnode->namesym, (INode*)fnode);
    if (foundnode) {
        errorMsgNode((INode*)fnode, ErrorDupName, "Duplicate name %s: Only methods can be overloaded.", &fnode->namesym->namestr);
        return;
    }
    nodelistAdd(&type->fields, (INode*)fnode);
}

// Serialize a struct type
void structPrint(StructNode *node) {
    inodeFprint(node->tag == StructTag? "struct %s {}" : "alloc %s {}", &node->namesym->namestr);
}

// Name resolution of a struct type
void structNameRes(NameResState *pstate, StructNode *node) {
    INode *svtypenode = pstate->typenode;
    pstate->typenode = (INode*)node;
    nametblHookPush();
    nametblHookNamespace(&node->namespace);
    if (node->basetrait)
        inodeNameRes(pstate, &node->basetrait);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        inodeNameRes(pstate, (INode**)nodesp);
    }
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        inodeNameRes(pstate, (INode**)nodesp);
    }
    nametblHookPop();
    pstate->typenode = svtypenode;
}

// Type check a struct type
void structTypeCheck(TypeCheckState *pstate, StructNode *node) {
    INode *svtypenode = pstate->typenode;
    pstate->typenode = (INode*)node;

    // If a base trait is specified, inherit its fields and methods
    if (node->basetrait) {
        inodeTypeCheck(pstate, &node->basetrait);
        StructNode *basenode = (StructNode*)itypeGetTypeDcl(node->basetrait);
        if (basenode->tag == StructTag && (basenode->flags & TraitType)) {
            INode **nodesp;
            uint32_t cnt;

            // Insert basetrait's fields ahead of this type's fields
            nodeListInsertList(&node->fields, &basenode->fields, 0);
            for (nodelistFor(&basenode->fields, cnt, nodesp)) {
                FieldDclNode *fld = (FieldDclNode*)*nodesp;
                INode *errfld;
                if (errfld = namespaceAdd(&node->namespace, fld->namesym, *nodesp)) {
                    errorMsgNode(errfld, ErrorDupName, "Duplicate field name with inherited trait field");
                }
            }

            // Renumber field indexes to reflect their altered position
            // Note: Since we are inserting at zero, the basetrait's indexes remain unchanged!
            uint16_t index = 0;
            for (nodelistFor(&node->fields, cnt, nodesp)) {
                ((FieldDclNode*)*nodesp)->index = index++;
            }
        }
        else {
            errorMsgNode(node->basetrait, ErrorInvType, "Expected a trait name.");
        }
    }

    // Type check all fields and calculate infection flags for ThreadBound/MoveType
    int16_t infectFlag = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        inodeTypeCheck(pstate, (INode**)nodesp);
        // Notice if a field's threadbound or movetype infects the struct
        ITypeNode *fldtype = (ITypeNode*)itypeGetTypeDcl(((IExpNode*)(*nodesp))->vtype);
        infectFlag |= fldtype->flags & (ThreadBound | MoveType);
    }
    // Use inference rules to decide if struct is ThreadBound or a MoveType
    // based on whether its fields are, and whether it supports the .final or .clone method
    if (namespaceFind(&node->namespace, finalName))
        infectFlag |= MoveType;           // Let's not make copies of finalized objects
    if (namespaceFind(&node->namespace, cloneName))
        infectFlag &= 0xFFFF - MoveType;  // 'clone' means we can make copies anyway
    node->flags |= infectFlag;

    // A 0-size (no field) struct is opaque. Cannot be instantiated.
    if (node->fields.used == 0)
        node->flags |= OpaqueType;

    // We can say we know enough about the type at this point to
    // declare it fully checked, so as to not trigger type recursion warnings.
    node->flags |= TypeChecked;

    // Type check all methods, etc.
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        inodeTypeCheck(pstate, (INode**)nodesp);
    }

    pstate->typenode = svtypenode;
}

// Compare two struct signatures to see if they are equivalent
int structEqual(StructNode *node1, StructNode *node2) {
    // inodes must match exactly in order
    return 1;
}

// Will from struct coerce to a to struct (we know they are not the same)
int structCoerces(StructNode *to, StructNode *from) {
    return 1;
}