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
    fldnode->lifeslots = 0;
    fldnode->lifeknown = 0;
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

// The type declared at module level that holds 'type': itself, or, for a
// variant written inside an enum's braces, the enum, whose visibility it has.
// NULL where no module owns it.
static StructNode *fieldDclModuleType(StructNode *type) {
    while (type->dclinfo.owner && type->dclinfo.owner->tag == StructTag)
        type = (StructNode*)type->dclinfo.owner;
    return type->dclinfo.owner && type->dclinfo.owner->tag == ModuleTag ? type : NULL;
}

// The first type private to its module that a field's type names, reached
// through references, pointers, arrays, tuples, type arguments and aliases, or
// NULL. A type parameter names no type yet: its instances are checked where
// they are named.
static StructNode *fieldDclPrivateType(INode *type) {
    if (type == NULL)
        return NULL;
    if (isNameUseNode(type))
        type = nameUseGetDcl((NameUseNode*)type);
    if (type == NULL)
        return NULL;
    switch (type->tag) {
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
        return fieldDclPrivateType(((RefNode*)type)->vtexp);
    case PtrTag:
        return fieldDclPrivateType(((StarNode*)type)->vtexp);
    case ArrayTag:
        return fieldDclPrivateType(arrayElemType(type));
    case TTupleTag:
    case FnCallTag: {
        if (type->tag == FnCallTag) {
            StructNode *found = fieldDclPrivateType(((FnCallNode*)type)->objfn);
            if (found || ((FnCallNode*)type)->args == NULL)
                return found;
        }
        Nodes *elems = type->tag == TTupleTag ? ((TupleNode*)type)->elems : ((FnCallNode*)type)->args;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(elems, cnt, nodesp)) {
            StructNode *found = fieldDclPrivateType(*nodesp);
            if (found)
                return found;
        }
        return NULL;
    }
    case AliasDclTag:
        return (type->flags & FlagTypeAlias) ? fieldDclPrivateType(((AliasDclNode*)type)->target) : NULL;
    case StructTag: {
        StructNode *top = fieldDclModuleType((StructNode*)type);
        return top && (top->dclinfo.facts & DclPrivate) ? top : NULL;
    }
    default:
        return NULL;
    }
}

// Enable name resolution of field declarations
void fieldDclNameRes(NameResState *pstate, FieldDclNode *name) {
    inodeNameRes(pstate, (INode**)&name->perm);
    inodeNameRes(pstate, &name->vtype);

    // A pub field of a pub type is reached from outside the module, so its type
    // must be one the outside can name. Privacy is the module's, members and
    // names alike: a private type's values never leave the module through a
    // field. A private type's pub field is not reached from outside through a
    // name, and is left alone.
    StructNode *holder = pstate->typenode && pstate->typenode->tag == StructTag
        ? fieldDclModuleType((StructNode*)pstate->typenode) : NULL;
    if ((name->flags & FlagPub) && holder && !(holder->dclinfo.facts & DclPrivate)) {
        StructNode *privtype = fieldDclPrivateType(name->vtype);
        if (privtype)
            errorMsgNode((INode*)name, ErrorPubFieldPrivType,
                "The field %s is pub, but its type %s is private to its module, so code outside the module would reach into a type it cannot name. Make the field private (its module still sees it), or declare %s pub.",
                &name->namesym->namestr, &privtype->namesym->namestr, &privtype->namesym->namestr);
    }

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
        // Fields require constant default values (litIsLiteral). A sibling
        // field's name is never one, and is refused as such before type check,
        // which with no self around a default could only say there is nothing
        // to reach it through.
        if (nameUseNames(name->value, FieldDclTag))
            errorMsgNode(name->value, ErrorNotLit, "Field default must be a constant value.");
        // Verify that declared type and initial value type matches
        else if (!iexpTypeCheckCoerce(pstate, name->vtype, &name->value))
            errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
        // The constant is judged after type check, as a const's value is: it is
        // type check that makes a construction, 'new E(1, 2)', or an array's
        // fill the literal it is, and type check that an expression of
        // constants is folded into the one it computes (litFoldConst)
        else if (!litFoldConst(&name->value))
            errorMsgNode(name->value, ErrorNotLit, "Field default must be a constant value.");
        else if (name->vtype == unknownType)
            name->vtype = ((IExpNode *)name->value)->vtype;
    }

    // A field holds its type by value, so that type has to be able to say how
    // large it is. This is where a recursive struct is caught -- and where one
    // that recurses through a reference is not, since the reference answers for
    // itself without asking what it points at. What a reference's target left
    // waiting in it -- an array's element size, an instance's layout, reached
    // first through a reference in this same layout -- is settled first, since
    // holding it by value is what demands it.
    structTypeSettle(pstate, name->vtype);
    INode *nosizeroot;
    char *nosize = itypeNoSizeCause(name->vtype, &nosizeroot);
    if (nosize) {
        errorMsgNode((INode*)name, ErrorNoSize, "Field %s cannot be held by value: %s %s.",
            &name->namesym->namestr, itypeName(nosizeroot), nosize);
        itypeNoSizeExplain(name->vtype);
    }
}
