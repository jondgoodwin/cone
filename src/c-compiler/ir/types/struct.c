/** Handling for structs
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <string.h>

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
    snode->derived = NULL;
    snode->vtable = NULL;
    snode->tagnbr = 0;
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

// Get bottom-most base trait for some trait/struct, or NULL if there is not one
StructNode *structGetBaseTrait(StructNode *node) {
    if (node->basetrait == NULL)
        return (node->flags & TraitType) ? node : NULL;
    return structGetBaseTrait((StructNode*)itypeGetTypeDcl(node->basetrait));
}

// Type check a struct type
void structTypeCheck(TypeCheckState *pstate, StructNode *node) {
    INode *svtypenode = pstate->typenode;
    pstate->typenode = (INode*)node;

    // If a base trait is specified, inject (inherit) its fields and methods
    if (node->basetrait) {
        inodeTypeCheck(pstate, &node->basetrait);
        StructNode *basenode = (StructNode*)itypeGetTypeDcl(node->basetrait);
        if (basenode->tag == StructTag && (basenode->flags & TraitType)) {
            INode **nodesp;
            uint32_t cnt;

            // Handle when base trait has a closed number of variants
            int16_t isClosedFlags = basenode->flags & (SameSize | HasTagField);
            if (isClosedFlags) {
                node->flags |= isClosedFlags;  // mark derived types with these flags
                if (basenode->mod != node->mod)
                    errorMsgNode((INode*)node, ErrorInvType, "This type must be declared in the same module as the trait");
                else {
                    // Remember every struct variant derived from a closed trait
                    // and capture struct's tag number
                    if (!(node->flags & TraitType)) {
                        StructNode *basesttrait = structGetBaseTrait(node);
                        if (basesttrait) {
                            if (basesttrait->derived == NULL)
                                basesttrait->derived = newNodes(4);
                            node->tagnbr = basesttrait->derived->used;
                            nodesAdd(&basesttrait->derived, (INode*)node);
                        }
                    }
                }
            }

            // Insert basetrait's fields ahead of this type's fields and in namespace
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

        if (fldtype->tag == EnumTag && !((*nodesp)->flags & IsTagField)) {
            if ((node->flags & TraitType) && !(node->flags & HasTagField) && node->basetrait == NULL) {
                node->flags |= HasTagField;
                (*nodesp)->flags |= IsTagField;
            }
            else {
                errorMsgNode(*nodesp, ErrorInvType, "Empty enum type only allowed once in a base trait");
            }
            if (((FieldDclNode*)(*nodesp))->namesym != anonName)
                errorMsgNode(*nodesp, ErrorInvType, "The tag discriminant field name should be '_'");
        }
    }
    // Use inference rules to decide if struct is ThreadBound or a MoveType
    // based on whether its fields are, and whether it supports the .final or .clone method
    if (namespaceFind(&node->namespace, finalName))
        infectFlag |= MoveType;           // Let's not make copies of finalized objects
    if (namespaceFind(&node->namespace, cloneName))
        infectFlag &= 0xFFFF - MoveType;  // 'clone' means we can make copies anyway

    // Populate infection flags in this struct/trait, and recursively to all inherited traits
    if (infectFlag) {
        node->flags |= infectFlag;
        StructNode *trait = (StructNode *)node->basetrait;
        while (trait) {
            trait->flags |= infectFlag;
            trait = (StructNode *)node->basetrait;
        }
    }

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

// Populate the vtable for this struct
void structMakeVtable(StructNode *node) {
    if (node->vtable)
        return;
    Vtable *vtable = memAllocBlk(sizeof(Vtable));
    vtable->llvmreftype = NULL;
    vtable->llvmvtable = NULL;
    vtable->impl = newNodes(4);

    vtable->name = memAllocStr(&node->namesym->namestr, node->namesym->namesz + 8);
    strcat(vtable->name, ":Vtable");

    // Populate methfld with all public methods and then fields in trait
    vtable->methfld = newNodes(node->fields.used + node->nodelist.used);
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        FnDclNode *meth = (FnDclNode *)*nodesp;
        if (meth->namesym->namestr != '_')
            nodesAdd(&vtable->methfld, *nodesp);
    }
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        INode *fieldtyp = itypeGetTypeDcl(field->vtype);
        if (field->namesym->namestr != '_' && fieldtyp->tag != EnumTag)
            nodesAdd(&vtable->methfld, *nodesp);
    }

    node->vtable = vtable;
}

// Populate the vtable implementation info for a struct ref being coerced to some trait
int structVirtRefMatches(StructNode *trait, StructNode *strnode) {

    if (trait->vtable == NULL)
        structMakeVtable(trait);
    Vtable *vtable = trait->vtable;


    // No need to build VtableImpl for this struct if it has already been done earlier
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(vtable->impl, cnt, nodesp)) {
        VtableImpl *impl = (VtableImpl*)*nodesp;
        if (impl->structdcl == (INode*)strnode)
            return 2;
    }

    // Create Vtable impl data structure and populate
    VtableImpl *impl = memAllocBlk(sizeof(VtableImpl));
    impl->llvmvtablep = NULL;
    impl->structdcl = (INode*)strnode;

    // Construct a global name for this vtable implementation
    size_t strsize = strnode->namesym->namesz + strlen(vtable->name) + 3;
    impl->name = memAllocStr(&strnode->namesym->namestr, strsize);
    strcat(impl->name, "->");
    strcat(impl->name, vtable->name);

    // For every field/method in the vtable, find its matching one in strnode
    impl->methfld = newNodes(vtable->methfld->used);
    for (nodesFor(vtable->methfld, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag) {
            // Locate the corresponding method with matching name and vtype
            // Note, we need to be flexible in matching the self parameter
            FnDclNode *meth = (FnDclNode *)*nodesp;
            FnDclNode *strmeth = (FnDclNode *)namespaceFind(&strnode->namespace, meth->namesym);
            if (strmeth == NULL || strmeth->tag != FnDclTag) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to a %s virtual reference. Missing method %s.",
                //    &strnode->namesym->namestr, &trait->namesym->namestr, &meth->namesym->namestr);
                return 0;
            }
            // Look through all overloaded methods for a match
            while (strmeth) {
                if (!itypeIsSame(strmeth->vtype, meth->vtype))
                    break;
                strmeth = strmeth->nextnode;
            }
            if (strmeth == NULL) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to a %s virtual reference. Incompatible type for method %s.",
                //    &strnode->namesym->namestr, &trait->namesym->namestr, &meth->namesym->namestr);
                return 0;
            }
            // it matches, add the method to the implementation
            nodesAdd(&impl->methfld, (INode*)strmeth);
        }
        else {
            // Find the corresponding field with matching name and vtype
            FieldDclNode *fld = (FieldDclNode *)*nodesp;
            INode *strfld = namespaceFind(&strnode->namespace, fld->namesym);
            if (strfld == NULL || strfld->tag != FieldDclTag) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to a %s virtual reference. Missing field %s.",
                //    &strnode->namesym->namestr, &trait->namesym->namestr, &fld->namesym->namestr);
                return 0;
            }
            if (!itypeIsSame(((FieldDclNode*)strfld)->vtype, fld->vtype)) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to a %s virtual reference. Incompatible type for field %s.",
                //    &strnode->namesym->namestr, &trait->namesym->namestr, &fld->namesym->namestr);
                return 0;
            }
            // it matches, add the corresponding field to the implementation
            nodesAdd(&impl->methfld, (INode*)strfld);
        }
    }

    // We accomplished a successful mapping - add it
    nodesAdd(&vtable->impl, (INode*)impl);
    return 2;
}

// Compare two struct signatures to see if they are equivalent
int structEqual(StructNode *node1, StructNode *node2) {
    // inodes must match exactly in order
    return 1;
}

// Will from struct coerce to a to struct (we know they are not the same)
// We can only do this for a same-sized trait supertype
int structMatches(StructNode *to, StructNode *from) {
    // Only matches if we coerce to a same-sized, nominally declared base trait
    if (!(to->flags & TraitType) || !(to->flags & SameSize))
        return 0;
    StructNode *base = (StructNode*)itypeGetTypeDcl(from->basetrait);
    while (base) {
        // If it is a valid supertype trait, indicate that it requires coercion
        if (to == base)
            return 2;
        base = (StructNode*)itypeGetTypeDcl(base->basetrait);
    }
    return 0;
}

// Will from struct coerce to a to struct (we know they are not the same)
// This works for references which have more relaxed subtyping rules
// because matching on size is not necessary
int structRefMatches(StructNode *to, StructNode *from) {
    if (!(to->flags & TraitType))
        return 0;
    StructNode *base = (StructNode*)itypeGetTypeDcl(from->basetrait);
    while (base) {
        // If it is a valid supertype trait, indicate that it requires coercion
        if (to == base)
            return 2;
        base = (StructNode*)itypeGetTypeDcl(base->basetrait);
    }
    return 0;
}