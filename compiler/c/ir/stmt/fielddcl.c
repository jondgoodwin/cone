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
    fldnode->vtype = unknownType;
    fldnode->namesym = namesym;
    fldnode->perm = perm;
    fldnode->value = NULL;
    fldnode->fold = NULL;
    fldnode->hop = NULL;
    fldnode->index = 0;
    return fldnode;
}

// Create an empty fold clause, positioned where the lexer is
FoldClause *newFoldClause() {
    FoldClause *fold = memAllocBlk(sizeof(FoldClause));
    INode *at;
    newNode(at, INode, KeywordTag);
    fold->at = at;
    fold->items = newNodes(4);
    fold->excludes = NULL;
    fold->star = 0;
    fold->expanded = 0;
    fold->ispub = 0;
    return fold;
}

// Create a new field node that is a copy of an existing one
INode *cloneFieldDclNode(CloneState *cstate, FieldDclNode *node) {
    FieldDclNode *newnode = memAllocBlk(sizeof(FieldDclNode));
    memcpy(newnode, node, sizeof(FieldDclNode));
    // A clone is unchecked however far along the node it was copied from got.
    // memcpy carries the type check marks with everything else, and a clone that
    // kept them would be skipped by the guard in inodeTypeCheck.
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    newnode->vtype = cloneNode(cstate, node->vtype);
    newnode->value = cloneNode(cstate, node->value);
    // The clone's clause is expanded afresh, into the clone's own namespace:
    // the copies and aliases the original's expansion made are the original's.
    // A listed item keeps its position; a star clause makes its items over.
    if (node->fold) {
        FoldClause *fold = memAllocBlk(sizeof(FoldClause));
        memcpy(fold, node->fold, sizeof(FoldClause));
        fold->expanded = 0;
        fold->items = node->fold->star ? newNodes(4) : cloneNodes(cstate, node->fold->items);
        fold->excludes = node->fold->excludes ? cloneNodes(cstate, node->fold->excludes) : NULL;
        newnode->fold = fold;
    }
    // A folded copy is never in a field list, so nothing clones one
    newnode->hop = NULL;
    return (INode*)newnode;
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
    if (name->fold) {
        INode **nodesp;
        uint32_t cnt;
        inodeFprint(name->fold->star ? " use *" : " use");
        if (name->fold->star && name->fold->excludes) {
            inodeFprint(" but");
            for (nodesFor(name->fold->excludes, cnt, nodesp))
                inodeFprint(cnt == name->fold->excludes->used ? " %s" : ", %s", &((NameUseNode*)*nodesp)->namesym->namestr);
        }
        else if (!name->fold->star) {
            for (nodesFor(name->fold->items, cnt, nodesp)) {
                AliasDclNode *alias = (AliasDclNode*)*nodesp;
                Name *from = ((NameUseNode*)alias->target)->namesym;
                inodeFprint(cnt == name->fold->items->used ? " %s" : ", %s", &from->namestr);
                if (alias->namesym != from)
                    inodeFprint(" as %s", &alias->namesym->namestr);
            }
        }
    }
    if (name->hop)
        inodeFprint(" (folded through %s)", &name->hop->namesym->namestr);
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
    inodeTypeCheckAny(pstate, (INode**)&name->perm);
    if (itypeTypeCheck(pstate, &name->vtype) == 0)
        return;

    // An initializer need not be specified, but if not, it must have a declared type
    if (!name->value) {
        if (name->vtype == unknownType) {
            errorMsgNode((INode*)name, ErrorNoType, "Declared field must specify a type or value");
            return;
        }
    }
    // Type check the initialization value
    else {
        // Fields require literal default values
        if (!litIsLiteral(name->value))
            errorMsgNode(name->value, ErrorNotLit, "Field default must be a literal value.");
        // Otherwise, verify that declared type and initial value type matches
        else if (!iexpTypeCheckCoerce(pstate, name->vtype, &name->value))
            errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
        else if (name->vtype == unknownType)
            name->vtype = ((IExpNode *)name->value)->vtype;
    }

    // A field holds its type by value, so that type has to be able to say how
    // large it is. This is where a recursive struct is caught -- and where one
    // that recurses through a reference is not, since the reference answers for
    // itself without asking what it points at.
    INode *nosizeroot;
    char *nosize = itypeNoSizeCause(name->vtype, &nosizeroot);
    if (nosize) {
        errorMsgNode((INode*)name, ErrorNoSize, "Field %s cannot be held by value: %s %s.",
            &name->namesym->namestr, itypeName(nosizeroot), nosize);
        itypeNoSizeExplain(name->vtype);
    }
}
