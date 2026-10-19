/** Handling for field destructuring nodes, 'x.(a, b)'
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <stdio.h>

// Create a new field destructuring of 'exp', its names still to be added
DestructNode *newDestructNode(INode *exp) {
    DestructNode *node;
    newNode(node, DestructNode, DestructTag);
    node->vtype = unknownType;
    node->exp = exp;
    node->names = newNodes(4);
    node->index = NULL;
    return node;
}

// Clone a field destructuring
INode *cloneDestructNode(CloneState *cstate, DestructNode *node) {
    DestructNode *newnode = memAllocBlk(sizeof(DestructNode));
    memcpy(newnode, node, sizeof(DestructNode));
    newnode->exp = cloneNode(cstate, node->exp);
    newnode->names = cloneNodes(cstate, node->names);
    if (node->index) {
        newnode->index = memAllocBlk(node->names->used * sizeof(uint32_t));
        memcpy(newnode->index, node->index, node->names->used * sizeof(uint32_t));
    }
    return (INode *)newnode;
}

void destructPrint(DestructNode *node) {
    INode **namep;
    uint32_t cnt;
    inodePrintNode(node->exp);
    inodeFprint(".(");
    for (nodesFor(node->names, cnt, namep)) {
        inodePrintNode(*namep);
        if (cnt > 1)
            inodeFprint(", ");
    }
    inodeFprint(")");
}

// The names are members of the value's type, which type check selects
void destructNameRes(NameResState *pstate, DestructNode *node) {
    inodeNameRes(pstate, &node->exp);
}

// A name written in the parentheses, for a message
static const char *destructNameText(INode *name, char *buf, size_t size) {
    if (name->tag == ULitTag)
        snprintf(buf, size, "%llu", (unsigned long long)((ULitNode *)name)->uintlit);
    else
        snprintf(buf, size, "%s", &((NameUseNode *)name)->namesym->namestr);
    return buf;
}

// The names must be all fields' or all tuple elements', and none repeated.
// Answers whether they are fine, after reporting each fault.
static int destructCheckNames(DestructNode *node) {
    Nodes *names = node->names;
    int ok = 1;
    INode **namep;
    uint32_t cnt;
    // (the parser reported a list with no names)
    if (names->used == 0)
        return 0;
    for (nodesFor(names, cnt, namep)) {
        if (!isNameUseNode(*namep) && (*namep)->tag != ULitTag) {
            errorMsgNode(*namep, ErrorFldListForm,
                "Name a field, `x.(a, b)`, or number a tuple's element, `t.(0, 2)`; this is neither.");
            ok = 0;
        }
    }
    if (!ok)
        return 0;
    int numbered = nodesGet(names, 0)->tag == ULitTag;
    for (uint32_t i = 0; i < names->used; ++i) {
        INode *name = nodesGet(names, i);
        if ((name->tag == ULitTag) != numbered) {
            errorMsgNode(name, ErrorFldListForm,
                "Name a struct's fields, `x.(a, b)`, or number a tuple's elements, `t.(0, 2)`; not both in one list.");
            return 0;
        }
        for (uint32_t j = 0; j < i; ++j) {
            INode *prior = nodesGet(names, j);
            int same = numbered
                ? ((ULitNode *)prior)->uintlit == ((ULitNode *)name)->uintlit
                : ((NameUseNode *)prior)->namesym == ((NameUseNode *)name)->namesym;
            if (same) {
                char buf[64];
                errorMsgNode(name, ErrorFldListDup,
                    "`%s` is named twice. Each field is taken once: name it once, and use the result's value where it is wanted twice.",
                    destructNameText(name, buf, sizeof(buf)));
                return 0;
            }
        }
    }
    return 1;
}

// Is this expression a place that keeps its value and can be named again
// without doing anything twice: a variable, or a field of or a dereference of
// one? Such a value is read field by field, as 'x.a' would read it.
static int destructIsPlace(INode *node) {
    if (isNameUseNode(node) && isExpNode(node))
        return 1;
    switch (node->tag) {
    case FldAccessTag:
        return destructIsPlace(((FnCallNode *)node)->objfn);
    case DerefTag:
        return destructIsPlace(((StarNode *)node)->vtexp);
    default:
        return 0;
    }
}

static INode *destructCopyNode(INode *node, size_t size) {
    INode *copy = (INode *)memAllocBlk(size);
    memcpy(copy, node, size);
    return copy;
}

// Another node naming the same place (destructIsPlace)
static INode *destructCopyPlace(INode *node) {
    if (isNameUseNode(node))
        return destructCopyNode(node, sizeof(NameUseNode));
    if (node->tag == FldAccessTag) {
        FnCallNode *copy = (FnCallNode *)destructCopyNode(node, sizeof(FnCallNode));
        copy->objfn = destructCopyPlace(((FnCallNode *)node)->objfn);
        INode *member = ((FnCallNode *)node)->methfld;
        if (member)
            copy->methfld = destructCopyNode(member, isNameUseNode(member) ? sizeof(NameUseNode) : sizeof(ULitNode));
        return (INode *)copy;
    }
    StarNode *copy = (StarNode *)destructCopyNode(node, sizeof(StarNode));
    copy->vtexp = destructCopyPlace(((StarNode *)node)->vtexp);
    return (INode *)copy;
}

// Read the fields as 'x.a' reads one: each name is a field access on the value,
// the value's place named again for each (a variable, a field of one), or kept
// first in a local of a block that gives the fields (a call's reference).
// One name gives that field, two or more a tuple of them. The accesses are
// checked as any are, so a reference is reached through, a copy type copies, a
// move type is refused as 'x.a' refuses it, and a private field is refused.
static void destructLower(TypeCheckState *pstate, INode **nodep) {
    DestructNode *node = (DestructNode *)*nodep;
    int place = destructIsPlace(node->exp);
    uint32_t count = node->names->used;
    VarDclNode *tmpvar = NULL;
    INode *first = node->exp;
    if (!place) {
        tmpvar = newVarDclFull(tempName, VarDclTag, ((IExpNode *)node->exp)->vtype, (INode *)immPerm, node->exp);
        inodeLexCopy((INode *)tmpvar, (INode *)node);
    }
    INode *result = NULL;
    TupleNode *tuple = count > 1 ? newTupleNode(count) : NULL;
    if (tuple) {
        tuple->tag = VTupleTag;
        inodeLexCopy((INode *)tuple, (INode *)node);
    }
    for (uint32_t i = 0; i < count; ++i) {
        INode *base;
        if (place)
            base = i == 0 ? first : destructCopyPlace(first);
        else {
            NameUseNode *use = newNameUseNode(tempName);
            use->vtype = tmpvar->vtype;
            use->dclnode = (INode *)tmpvar;
            inodeLexCopy((INode *)use, (INode *)node);
            base = (INode *)use;
        }
        FnCallNode *access = newFnCallNode(base, 0);
        inodeLexCopy((INode *)access, nodesGet(node->names, i));
        access->methfld = nodesGet(node->names, i);
        access->flags |= FlagRcvChecked;
        if (tuple)
            nodesAdd(&tuple->elems, (INode *)access);
        else
            result = (INode *)access;
    }
    if (tuple)
        result = (INode *)tuple;
    inodeTypeCheckAny(pstate, &result);
    if (!place && isExpNode(result) && ((IExpNode *)result)->vtype != errorType) {
        BlockNode *blk = newBlockNode();
        inodeLexCopy((INode *)blk, (INode *)node);
        blk->vtype = ((IExpNode *)result)->vtype;
        blk->flags |= FlagKeepTemps;
        nodesAdd(&blk->stmts, (INode *)tmpvar);
        nodesAdd(&blk->stmts, result);
        result = (INode *)blk;
    }
    *nodep = result;
}

// The fields of a struct that a list may name, for the message that lists them
static void destructFieldList(StructNode *strnode, char *buf, size_t size) {
    size_t used = 0;
    INode **fldp;
    uint32_t cnt;
    buf[0] = '\0';
    for (nodelistFor(&strnode->fields, cnt, fldp)) {
        FieldDclNode *fld = (FieldDclNode *)*fldp;
        if ((fld->flags & (IsTagField | IsMixin)) || fld->namesym == NULL)
            continue;
        int n = snprintf(buf + used, size - used, "%s%s", used ? ", " : "", &fld->namesym->namestr);
        if (n < 0 || (size_t)n >= size - used)
            break;
        used += (size_t)n;
    }
}

// Do the names fit the struct or tuple 'type'? A struct's names are fields of
// its own, a tuple's numbers are elements it has. With 'take', the value is
// taken apart -- all of it moved in (flow) and the fields not named dying with the
// statement (generation) -- so the type must have no finalizer, a private field
// is refused outside its module, and the node is given each name's index and its
// type. Answers whether the names are fine, after reporting the fault.
static int destructNamesCheck(TypeCheckState *pstate, DestructNode *node, INode *type, int take) {
    uint32_t count = node->names->used;
    if (take)
        node->index = memAllocBlk(count * sizeof(uint32_t));
    INode *objtype = ((IExpNode *)node->exp)->vtype;
    INode **fieldtypes = memAllocBlk(count * sizeof(INode *));
    if (type->tag == TTupleTag) {
        Nodes *elems = ((TupleNode *)type)->elems;
        for (uint32_t i = 0; i < count; ++i) {
            INode *name = nodesGet(node->names, i);
            if (name->tag != ULitTag) {
                errorMsgNode(name, ErrorFldListForm,
                    "A tuple's elements are numbered from 0, `t.(0, 2)`; `%s` is a name.",
                    &((NameUseNode *)name)->namesym->namestr);
                return 0;
            }
            uint64_t at = ((ULitNode *)name)->uintlit;
            if (at >= elems->used) {
                errorMsgNode(name, ErrorFldListUnknown,
                    "This tuple has %d elements, numbered 0 to %d; there is no element %llu.",
                    (int)elems->used, (int)elems->used - 1, (unsigned long long)at);
                return 0;
            }
            if (take)
                node->index[i] = (uint32_t)at;
            fieldtypes[i] = nodesGet(elems, at);
        }
    }
    else {
        StructNode *strnode = (StructNode *)type;
        // A finalizer needs the value whole: it would run on a value whose
        // fields have gone
        if (take && namespaceFind(&strnode->namespace, finalName)) {
            errorMsgNode((INode *)node, ErrorFldListFinal,
                "%s has a finalizer, so it cannot be taken apart: its finalizer needs all of its fields. Read its fields through a borrow, `r.a`, or move the whole value.",
                &strnode->namesym->namestr);
            return 0;
        }
        for (uint32_t i = 0; i < count; ++i) {
            INode *name = nodesGet(node->names, i);
            if (name->tag == ULitTag) {
                errorMsgNode(name, ErrorFldListForm,
                    "A struct's fields are named, `x.(a, b)`; only a tuple's elements are numbered.");
                return 0;
            }
            Name *namesym = ((NameUseNode *)name)->namesym;
            INode *found = iNsTypeFindFnField((INsTypeNode *)strnode, namesym);
            if (found == NULL || found->tag != FieldDclTag || !(found->flags & FlagMethFld)
                || ((FieldDclNode *)found)->hop != NULL) {
                if (found != NULL && (found->tag == FieldDclTag || found->tag == AliasDclTag)
                    && (found->tag == AliasDclTag || ((FieldDclNode *)found)->hop != NULL)) {
                    errorMsgNode(name, ErrorFldListUnknown,
                        "`%s` is a field %s holds through another of its fields, and is not one of its own. Name the field that holds it and take it from there.",
                        &namesym->namestr, &strnode->namesym->namestr);
                    return 0;
                }
                char list[512];
                destructFieldList(strnode, list, sizeof(list));
                errorMsgNode(name, ErrorFldListUnknown, "`%s` is not a field of %s. Its fields are: %s.",
                    &namesym->namestr, &strnode->namesym->namestr, list);
                return 0;
            }
            // Module-wide privacy, as for any field (fnCallLowerMethodOn)
            if (take && inodeIsPrivate(found) && !structSeesPrivate(pstate, (INode *)strnode)) {
                errorMsgNode(name, ErrorNotPublic, "May not access the private method/field `%s`.", &namesym->namestr);
                return 0;
            }
            FieldDclNode *fld = (FieldDclNode *)found;
            if (take)
                node->index[i] = fld->index;
            fieldtypes[i] = take ? lifeBrandField(objtype, fld->vtype, (INode *)node) : NULL;
        }
    }
    if (!take)
        return 1;
    if (count == 1)
        node->vtype = fieldtypes[0];
    else {
        TupleNode *result = newTupleNode(count);
        result->tag = TTupleTag;
        for (uint32_t i = 0; i < count; ++i)
            nodesAdd(&result->elems, fieldtypes[i]);
        node->vtype = (INode *)result;
    }
    return 1;
}

// Type check a field destructuring. Taken apart only a value that is consumed:
// a move type's, or any value no place keeps. A copy type kept in a place, and
// everything reached through a reference, is read as 'x.a' reads it.
void destructTypeCheck(TypeCheckState *pstate, INode **nodep) {
    DestructNode *node = (DestructNode *)*nodep;
    node->vtype = errorType;
    int namesok = destructCheckNames(node);
    if (iexpTypeCheckAny(pstate, &node->exp) == 0 || inodeIsError(node->exp) || ((IExpNode *)node->exp)->vtype == errorType) {
        *nodep = newErrorNode((INode *)node);
        return;
    }
    if (!namesok) {
        *nodep = newErrorNode((INode *)node);
        return;
    }
    INode *type = iexpGetTypeDcl(node->exp);
    int isvalue = type->tag == TTupleTag
        || (type->tag == StructTag && !(type->flags & (TraitType | EnumType)));
    int take = isvalue && !(destructIsPlace(node->exp) && !iexpIsMove(node->exp));
    // What the names are looked up in: a struct or a tuple, itself or what a
    // reference or a pointer points at. Other types answer as 'x.a' does.
    INode *held = type;
    while (held->tag == RefTag || held->tag == PtrTag)
        held = itypeGetTypeDcl(held->tag == RefTag ? ((RefNode *)held)->vtexp : ((StarNode *)held)->vtexp);
    // A number, an array, a function: no fields to name. (A generic's body is
    // checked for the type it is instantiated with, so a type parameter whose
    // type has none, or lacks the field, is refused here or below.)
    switch (held->tag) {
    case IntNbrTag: case UintNbrTag: case FloatNbrTag: case ArrayTag: case ArrayRefTag:
    case FnSigTag: case VoidTag: case GenVarDclTag:
        errorMsgNode((INode *)node, ErrorFldListType,
            "%s has no fields to take: a field list names the fields of a struct, `x.(a, b)`, or the elements of a tuple, `t.(0, 2)`.",
            itypeName(held));
        *nodep = newErrorNode((INode *)node);
        return;
    default:
        break;
    }
    if ((held->tag == TTupleTag || (held->tag == StructTag && !(held->flags & (TraitType | EnumType))))
        && !destructNamesCheck(pstate, node, held, take)) {
        *nodep = newErrorNode((INode *)node);
        return;
    }
    if (!take)
        destructLower(pstate, nodep);
}
