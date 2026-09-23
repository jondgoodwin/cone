/** Handling for structs
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <string.h>
#include <assert.h>

// Create a new struct type whose info will be filled in afterwards
StructNode *newStructNode(Name *namesym) {
    StructNode *snode;
    newNode(snode, StructNode, StructTag);
    snode->namesym = namesym;
    snode->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)snode, 8);
    nodelistInit(&snode->fields, 8);
    dclInfoInit(&snode->dclinfo);
    snode->basetrait = NULL;
    snode->derived = NULL;
    snode->traits = NULL;
    snode->vtable = NULL;
    snode->genericinfo = NULL;
    snode->tagnbr = 0;
    return snode;
}

// Clone struct
INode *cloneStructNode(CloneState *cstate, StructNode *node) {
    StructNode *newnode = memAllocBlk(sizeof(StructNode));
    memcpy(newnode, node, sizeof(StructNode));
    newnode->genericinfo = NULL;
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);

    // Within the copy, 'Self' is the copy. A method's self parameter is declared
    // as a use of 'Self' (parsetype.c), and name resolution has already pointed
    // that use at the struct being copied -- so without this every method of
    // every generic instance kept the generic's type as its receiver, matching
    // neither the instance at its declaration nor the call that selects it.
    // The copy exists before any of its members are cloned, which is what makes
    // this the place to say so. Saved and restored because a struct may be
    // cloned while some enclosing 'Self' is in force.
    INode *svselftype = cstate->selftype;
    cstate->selftype = (INode*)newnode;

    // Fields like derived, vtable, tagnbr do not yet have useful data to clone
    newnode->basetrait = cloneNode(cstate, node->basetrait);
    if (node->derived)
        newnode->derived = newNodes(node->derived->used);
    // The traits name resolution mixed into the template are the instance's
    // too, since their members are cloned below with everything else. The list
    // is the instance's own, because a generic base trait is mixed in only once
    // the instance exists (structTypeCheck) and is appended here.
    if (node->traits) {
        newnode->traits = newNodes(node->traits->used);
        INode **traitp;
        uint32_t traitcnt;
        for (nodesFor(node->traits, traitcnt, traitp))
            nodesAdd(&newnode->traits, *traitp);
    }

    // Recreate clones of fields/mixins and methods, sequentially and in namespace dictionary
    namespaceInit(&newnode->namespace, node->namespace.avail);
    INode **newnodesp = (INode**)memAllocBlk(node->fields.avail * sizeof(INode *));
    newnode->fields.nodes = newnodesp;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        *newnodesp = cloneNode(cstate, *nodesp);
        if ((*newnodesp)->tag == FieldDclTag) {
            FieldDclNode *newfld = (FieldDclNode*)*newnodesp;
            namespaceAdd(&newnode->namespace, newfld->namesym, (INode*)newfld);
        }
        ++newnodesp;
    }
    // The copy's method list has to start empty. memcpy carried the original's
    // 'used' count across, and iNsTypeAddFn appends at that count -- so writing
    // each clone into a fresh array as well listed every method twice, once at
    // its own index and once appended after the copied count. structTypeCheck
    // walks this list to type check bodies, so every method of every instance
    // was type checked twice on the same node, and the second visit re-lowered
    // what the first had already lowered: an operator call, by then a use of the
    // operator's own FnDcl, took the "rewrite to self.method" path and was
    // rejected as a method the instance does not declare.
    nodelistInit(&newnode->nodelist, node->nodelist.avail);
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        INode *member = cloneNode(cstate, *nodesp);
        if (member->tag == MacroDclTag)
            iNsTypeAddMacro((INsTypeNode*)newnode, (MacroDclNode*)member);
        else if (member->tag == VarDclTag)
            iNsTypeAddStatic((INsTypeNode*)newnode, (VarDclNode*)member);
        else
            iNsTypeAddFn((INsTypeNode*)newnode, (FnDclNode*)member);
    }

    cstate->selftype = svselftype;
    return (INode *)newnode;
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
    inodeFprint(node->tag == StructTag? "struct %s" : "alloc %s", &node->namesym->namestr);
    if (node->genericinfo)
        genericInfoPrint(node->genericinfo);
    dclInfoPrint((INode*)node);
    // Each method by name and owner only: an inherited default or a generic
    // instance's method is owned by this type, not by where it was written
    inodeFprint("{");
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        Name *namesym = inodeGetName(*nodesp);
        char *kind = (*nodesp)->tag == MacroDclTag ? "macro" : (*nodesp)->tag == VarDclTag ? "static" : "fn";
        inodeFprint(cnt == node->nodelist.used ? "%s %s" : ", %s %s", kind, namesym ? &namesym->namestr : "");
        dclInfoPrint(*nodesp);
    }
    inodeFprint("}");
}

// Does this base contribute its fields to the type standing on it?
//
// An enum does: it owns its variants' layout, so a variant's fields begin with
// clones of the enum's -- the discriminant among them -- and the variant declares
// none of them. A trait does not: what a trait's fields state is a requirement
// the type satisfies by declaring them itself, which is what 'is' verifies.
static int structBaseGivesFields(StructNode *base) {
    return (base->flags & EnumType) != 0;
}

// Expand the placeholder field at 'fldpos' that stands for a base or a mixin:
// the base's fields replace it as clones where it contributes any, and otherwise
// it is removed, since a trait adds nothing to the layout. Each method the trait
// gives a body to is cloned into this type's method list unless the type declares
// the name itself. A required method the type does declare is left for type check
// to compare against the requirement, since that needs the signatures' types. The
// trait is recorded so type check can find every requirement it imposes.
//
// Called from name resolution for a trait that is a declaration when the type
// is resolved, and from type check for an instance of a generic trait, which is
// not one until the instantiation is type checked. Either way the trait's own
// members are already resolved, so the clones arrive bound.
static void structInheritTrait(StructNode *node, uint32_t fldpos, StructNode *trait, CloneState *cstate) {
    INode **nodesp;
    uint32_t cnt;

    if (!structBaseGivesFields(trait))
        nodelistMakeSpace(&node->fields, fldpos, -1);
    else {
        // Replace the placeholder with all the enum's fields
        nodelistMakeSpace(&node->fields, fldpos, trait->fields.used - 1);
        INode **insertp = &nodelistGet(&node->fields, fldpos);
        for (nodelistFor(&trait->fields, cnt, nodesp)) {
            FieldDclNode *newfld = (FieldDclNode*)cloneNode(cstate, *nodesp);
            *insertp++ = (INode *)newfld;
            if (namespaceAdd(&node->namespace, newfld->namesym, (INode*)newfld)) {
                errorMsgNode((INode*)newfld, ErrorDupName, "Enum may not splice in a duplicate field name");
            }
        }
    }

    // Fold in the trait's default methods
    for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag)
            continue;
        // Only a method is inherited. A static function of the trait takes no
        // receiver, so there is nothing about it to specialize per implementer
        // and nothing that dispatches it: it stays the trait's own, reached as
        // 'Trait.name'. Copying it gave every implementer a symbol no name could
        // reach, since a qualified name on the implementer does not find it either.
        if (!((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *traitmeth = (FnDclNode*)*nodesp;
        // A requirement with no body is inherited as it is, so that the name is
        // in the namespace: a trait passes the requirement on to its own
        // implementers, and a struct is told to implement it by type check.
        if (iNsTypeFindFnField((INsTypeNode *)node, traitmeth->namesym) == NULL)
            iNsTypeAddFn((INsTypeNode *)node, (FnDclNode*)cloneNode(cstate, (INode*)traitmeth));
    }

    if (node->traits == NULL)
        node->traits = newNodes(2);
    nodesAdd(&node->traits, (INode*)trait);
}

// The trait declaration a base or mixin type expression names, or NULL when it
// names something else: a generic instantiation, which is a call node until type
// check instantiates it; a type that is not a trait, which type check reports; or
// a name that did not resolve. The base of an 'is' must be an abstraction, and
// this is the test in force at name resolution -- an enum answers it too, since a
// variant's membership rides the same field.
static StructNode *structNameResTrait(INode *typeexp) {
    if (typeexp->tag == FnCallTag || !isTypeNode(typeexp))
        return NULL;
    INode *dcl = itypeGetTypeDcl(typeexp);
    if (dcl->tag != StructTag || !(dcl->flags & TraitType))
        return NULL;
    return (StructNode*)dcl;
}

// Resolve a type's declaration now, ahead of the walk, because another type's
// resolution needs its members: what a type is-a or mixes in must have its
// own members in place before they are read. Returns 0 when the type is
// already being resolved, which means the two depend on each other.
//
// Demand is confined to type declarations reached from type declarations, so
// what is hooked at the jump is known: module names, and the demanding type's
// generic parameters. A type declared in another module resolves in that
// module's own scope: its namespace is hooked over the current one, and when
// the module has not begun its own resolution -- modules resolve in load order
// and the root loads first -- the names its wildcard imports will fold are
// hooked too, without being folded, so that the module's namespace is exactly
// what its own resolution makes it.
int structNameResDemand(NameResState *pstate, StructNode *type) {
    if (type->flags & NameResolved)
        return 1;
    if (type->flags & NameResolving)
        return 0;
    ModuleNode *mod = dclInfoGetModule((INode*)type);
    if (mod == NULL || mod == pstate->mod) {
        structNameRes(pstate, type);
        return 1;
    }
    ModuleNode *svmod = pstate->mod;
    pstate->mod = mod;
    modHook(NULL, mod);
    if (!(mod->flags & (NameResolved | NameResolving))) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(mod->imports, cnt, nodesp))
            importHookFolds((ImportNode*)*nodesp);
    }
    structNameRes(pstate, type);
    modHook(mod, NULL);
    pstate->mod = svmod;
    return 1;
}

// ---- Name folding: a field's 'use' clause ----------------------------------
//
// A fold clause admits names of the field's type as names of this type. A
// folded field becomes a copy in this type's namespace: the field's own type,
// permission and index, plus a hop to the field of this type it is reached
// through, so an access to it is lowered to the access path written out. A
// folded method, overload set or macro method becomes an alias whose target
// is bound to the declaration; a call resolves the alias and shifts its
// receiver to the field (structFoldReceiver). Expansion runs in name
// resolution once the field's type is a declaration, so that a method body may
// name a folded member bare, and in type check for an instance of a generic,
// whose field types exist only then.

// The declaration of the field's type, through a reference or pointer if the
// field holds one, or NULL when it is not a declaration yet: an instance of a
// generic still to be instantiated, or a name that did not resolve.
static INode *structFoldSourceDcl(FieldDclNode *field) {
    INode *vtype = field->vtype;
    if (vtype->tag == FnCallTag || !isTypeNode(vtype))
        return NULL;
    INode *dcl = itypeGetTypeDcl(vtype);
    if (dcl->tag == RefTag || dcl->tag == VirtRefTag)
        vtype = ((RefNode*)dcl)->vtexp;
    else if (dcl->tag == PtrTag)
        vtype = ((StarNode*)dcl)->vtexp;
    else
        return dcl;
    if (vtype->tag == FnCallTag || !isTypeNode(vtype))
        return NULL;
    return itypeGetTypeDcl(vtype);
}

// Is this name one a clause's 'but' leaves out?
static int structFoldExcluded(FoldClause *fold, Name *name) {
    if (fold->excludes == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(fold->excludes, cnt, nodesp))
        if (((NameUseNode*)*nodesp)->namesym == name)
            return 1;
    return 0;
}

// The alias a clause of 'type' admits under 'name', and the field carrying
// that clause, or NULL when no clause of the type admits it. Unique when it
// exists, since a folded name may collide with nothing.
static AliasDclNode *structFoldItemOf(StructNode *type, Name *name, FieldDclNode **fieldp) {
    INode **fldp;
    uint32_t fldcnt;
    for (nodelistFor(&type->fields, fldcnt, fldp)) {
        FieldDclNode *field = (FieldDclNode*)*fldp;
        if (field->fold == NULL)
            continue;
        INode **itemp;
        uint32_t itemcnt;
        for (nodesFor(field->fold->items, itemcnt, itemp)) {
            if (((AliasDclNode*)*itemp)->namesym == name) {
                *fieldp = field;
                return (AliasDclNode*)*itemp;
            }
        }
    }
    return NULL;
}

// A copy in this type of the field 'orig' of the fold's source type, reached
// through 'field': orig's own type, permission and index, with a hop to a copy
// of orig's own hop, or to 'field' itself where orig is a declared field. So
// a chain of folds is copied whole, and the chain always ends at a declared
// field of this type. Positioned on the fold item, so a diagnostic lands there.
static FieldDclNode *structFoldCopy(FieldDclNode *orig, FieldDclNode *field, INode *at) {
    FieldDclNode *copy = memAllocBlk(sizeof(FieldDclNode));
    memcpy(copy, orig, sizeof(FieldDclNode));
    inodeLexCopy((INode*)copy, at);
    copy->flags = (copy->flags | FlagMethFld | FlagPub) & (0xffff - (TypeChecked | TypeChecking | IsTagField | IsMixin));
    copy->fold = NULL;
    copy->hop = orig->hop ? structFoldCopy(orig->hop, field, at) : field;
    return copy;
}

// Expand one item of a fold clause: bind its target in the source type and
// enter it in this type's namespace, as a copy for a field and as the alias
// itself for a method, overload set or macro method.
static void structFoldItem(StructNode *node, FieldDclNode *field, StructNode *src, AliasDclNode *alias, int hook) {
    NameUseNode *target = (NameUseNode*)alias->target;
    Name *srcname = target->namesym;
    INode *found = namespaceFind(&src->namespace, srcname);
    if (found == NULL) {
        errorMsgNode((INode*)alias, ErrorNoMbr, "%s has no member named %s to fold in.",
            &src->namesym->namestr, &srcname->namestr);
        return;
    }
    // Visibility is transitive: only what the field's type shows is folded
    if (inodeIsPrivate(found)) {
        errorMsgNode((INode*)alias, ErrorNotPublic, "%s is private to %s, so it does not fold.",
            &srcname->namestr, &src->namesym->namestr);
        return;
    }
    // Through the source's own aliases to the declaration; a source alias the
    // source's fold failed to bind was reported there
    INode *dcl = aliasDclResolve(found);
    if (dcl == NULL)
        return;
    INode *entry;
    switch (dcl->tag) {
    case FieldDclTag: {
        FieldDclNode *copy = structFoldCopy((FieldDclNode*)dcl, field, (INode*)alias);
        copy->namesym = alias->namesym;
        target->dclnode = (INode*)copy;
        entry = (INode*)copy;
        break;
    }
    case FnDclTag:
    case FnOverloadDclTag:
    case MacroDclTag:
        // A static is reached through the type rather than a value, so there
        // is no receiver to shift; a finalizer or clone is the value's own
        if (!inodeIsMember(dcl)) {
            errorMsgNode((INode*)alias, ErrorBadFold, "%s is a static of %s: it is reached through the type, not through a value, so it does not fold.",
                &srcname->namestr, &src->namesym->namestr);
            return;
        }
        if (srcname == finalName || srcname == cloneName) {
            errorMsgNode((INode*)alias, ErrorBadFold, "%s belongs to %s's own lifecycle, so it does not fold.",
                &srcname->namestr, &src->namesym->namestr);
            return;
        }
        target->dclnode = dcl;
        entry = (INode*)alias;
        break;
    default:
        errorMsgNode((INode*)alias, ErrorBadFold, "%s is not a field or method of %s, so it does not fold.",
            &srcname->namestr, &src->namesym->namestr);
        return;
    }
    INode *prior = namespaceAdd(&node->namespace, alias->namesym, entry);
    if (prior) {
        errorMsgNode((INode*)alias, ErrorDupName, "%s is already a name of %s. A folded name must be unique: rename it with 'as', or leave it out with 'but'.",
            &alias->namesym->namestr, &node->namesym->namestr);
        return;
    }
    if (hook)
        nametblHookNode(alias->namesym, entry);
}

// Make the items of a 'use *' clause: an alias for every public member of the
// source type not left out by 'but' -- fields and methods, its own folded
// copies and aliases included, so a fold chains through the types. Not a
// static, a macro without self, Self, or the value's own finalizer or clone.
static void structFoldStar(StructNode *node, FieldDclNode *field, StructNode *src) {
    FoldClause *fold = field->fold;
    INode **nodesp;
    uint32_t cnt;
    if (fold->excludes) {
        for (nodesFor(fold->excludes, cnt, nodesp)) {
            Name *name = ((NameUseNode*)*nodesp)->namesym;
            if (namespaceFind(&src->namespace, name) == NULL)
                errorMsgNode(*nodesp, ErrorNoMbr, "%s has no member named %s to leave out.",
                    &src->namesym->namestr, &name->namestr);
        }
    }
    namespaceFor(&src->namespace) {
        NameNode *nn = &src->namespace.namenodes[__i];
        if (nn->name == NULL || nn->name == selfTypeName || nn->name == anonName
            || nn->name == finalName || nn->name == cloneName)
            continue;
        if (inodeIsPrivate(nn->node) || !inodeIsMember(nn->node) || structFoldExcluded(fold, nn->name))
            continue;
        NameUseNode *target = newMemberUseNode(nn->name);
        inodeLexCopy((INode*)target, fold->at);
        AliasDclNode *alias = newAliasDclNode(nn->name, (INode*)target);
        inodeLexCopy((INode*)alias, fold->at);
        nodesAdd(&fold->items, (INode*)alias);
    }
}

// Expand a field's fold clause into this type's namespace, hooking each entry
// when name resolution asks. Nothing happens while the field's type is not yet
// a declaration; the clause is then expanded when the instance is type checked.
static void structFoldExpand(StructNode *node, FieldDclNode *field, int hook) {
    FoldClause *fold = field->fold;
    INode *srcdcl = structFoldSourceDcl(field);
    if (srcdcl == NULL)
        return;
    fold->expanded = 1;
    // Visibility is transitive: a folded name is reached through the field
    if (inodeIsPrivate((INode*)field)) {
        errorMsgNode(fold->at, ErrorNotPublic, "Only a pub field folds names in: a folded name is reached through the field, and %s is private.",
            &field->namesym->namestr);
        return;
    }
    if (srcdcl->tag != StructTag) {
        errorMsgNode(fold->at, ErrorBadFold, "A fold takes its names from a struct, and the type of %s is not one.",
            &field->namesym->namestr);
        return;
    }
    StructNode *src = (StructNode*)srcdcl;
    if (src->flags & TraitType) {
        errorMsgNode(fold->at, ErrorBadFold, "%s is a trait. A fold reaches through a value's own members, and an abstraction has none to reach.",
            &src->namesym->namestr);
        return;
    }
    // A fold needs the field's type complete: its own folds expanded, so that
    // a fold chains, which a type still under way cannot offer
    if (src == node) {
        errorMsgNode(fold->at, ErrorCircular, "%s cannot fold from itself: a fold needs the field's type complete first.",
            &node->namesym->namestr);
        return;
    }
    if (!(src->flags & NameResolved)) {
        errorMsgNode(fold->at, ErrorCircular, "A fold needs %s complete, and %s is not complete until %s is.",
            &src->namesym->namestr, &src->namesym->namestr, &node->namesym->namestr);
        return;
    }
    if (fold->star)
        structFoldStar(node, field, src);
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(fold->items, cnt, itemp))
        structFoldItem(node, field, src, (AliasDclNode*)*itemp, hook);
}

// Bring a folded copy up to date with the declared field it stands for. A copy
// is made when the fold is expanded, in name resolution, and takes the field's
// type node and index as they are then; type check may replace that node (an
// instantiation of a generic becomes the instance) and re-index the fields.
// So once every field of this type is checked, each copy takes its origin's
// type, permission and index over again, the origin being demanded first: a
// copy in the source type is refreshed the same way, and a declared field is
// type checked, wherever its own type's check has got to.
static void structFoldRefreshCopy(TypeCheckState *pstate, StructNode *type, AliasDclNode *item, FieldDclNode *field) {
    NameUseNode *target = (NameUseNode*)item->target;
    FieldDclNode *copy = (FieldDclNode*)target->dclnode;
    if (copy == NULL || copy->tag != FieldDclTag || copy->hop == NULL)
        return;
    INode *srcdcl = structFoldSourceDcl(field);
    if (srcdcl == NULL || srcdcl->tag != StructTag)
        return;
    StructNode *src = (StructNode*)srcdcl;
    INode *origbind = namespaceFind(&src->namespace, target->namesym);
    if (origbind == NULL || origbind->tag != FieldDclTag)
        return;
    FieldDclNode *orig = (FieldDclNode*)origbind;
    if (orig->hop) {
        FieldDclNode *srcfield;
        AliasDclNode *srcitem = structFoldItemOf(src, target->namesym, &srcfield);
        if (srcitem)
            structFoldRefreshCopy(pstate, src, srcitem, srcfield);
    }
    else {
        INode *origp = (INode*)orig;
        inodeTypeCheckAny(pstate, &origp);
    }
    for (; orig; orig = orig->hop, copy = copy->hop) {
        copy->vtype = orig->vtype;
        copy->perm = orig->perm;
        copy->index = orig->index;
    }
}

// Refresh every folded copy of this type (above)
static void structFoldRefresh(TypeCheckState *pstate, StructNode *node) {
    INode **fldp;
    uint32_t fldcnt;
    for (nodelistFor(&node->fields, fldcnt, fldp)) {
        FieldDclNode *field = (FieldDclNode*)*fldp;
        if (field->fold == NULL || !field->fold->expanded)
            continue;
        INode **itemp;
        uint32_t itemcnt;
        for (nodesFor(field->fold->items, itemcnt, itemp))
            structFoldRefreshCopy(pstate, node, (AliasDclNode*)*itemp, field);
    }
}

// Walk the receiver of a call to a method 'type' holds by folding down to the
// field the method was folded through: the access to the field whose clause
// admits the name, then on into that field's type where the name is folded
// there too. The clauses of each type are read in place; nothing about the
// method is copied.
static void structFoldReceiverWalk(StructNode *type, Name *name, INode **objp, INode *lexnode) {
    FieldDclNode *field;
    AliasDclNode *item = structFoldItemOf(type, name, &field);
    if (item == NULL)
        return;
    *objp = fnCallFieldAccess(*objp, field, lexnode);
    INode *srcdcl = structFoldSourceDcl(field);
    if (srcdcl == NULL || srcdcl->tag != StructTag)
        return;
    Name *srcname = ((NameUseNode*)item->target)->namesym;
    INode *entry = namespaceFind(&((StructNode*)srcdcl)->namespace, srcname);
    if (entry && entry->tag == AliasDclTag)
        structFoldReceiverWalk((StructNode*)srcdcl, srcname, objp, lexnode);
}

// Rewrite the receiver of a call to a method 'type' holds by folding (above).
// A reference receiver dereferences and reborrows the field it lands on with
// the reference's own permission, so a method wanting 'self &mut' is reached
// through '&mut c' exactly as through '&mut c.engine' written out; a field
// that is itself a reference is the receiver as it stands. A value receiver
// stays a value, as the path written out would, and reaches only what a value
// reaches: a method taking self by value. The fold adds no rule of its own.
void structFoldReceiver(StructNode *type, Name *name, INode **objp, INode *lexnode) {
    INode *objtype = iexpGetTypeDcl(*objp);
    structFoldReceiverWalk(type, name, objp, lexnode);
    if (objtype->tag == RefTag) {
        INode *fldtype = iexpGetTypeDcl(*objp);
        if (fldtype->tag != RefTag && fldtype->tag != PtrTag && fldtype->tag != VirtRefTag)
            borrowMutRef(objp, ((IExpNode*)*objp)->vtype, ((RefNode*)objtype)->perm);
    }
}

// The fields a folded method's receiver is reached through, outermost first:
// what structFoldReceiver walks, recorded for a vtable slot's thunk. NULL for
// a name the type declares itself.
static Nodes *structFoldPath(StructNode *type, Name *name, Nodes *path) {
    FieldDclNode *field;
    AliasDclNode *item = structFoldItemOf(type, name, &field);
    if (item == NULL)
        return path;
    if (path == NULL)
        path = newNodes(2);
    nodesAdd(&path, (INode*)field);
    INode *srcdcl = structFoldSourceDcl(field);
    if (srcdcl == NULL || srcdcl->tag != StructTag)
        return path;
    Name *srcname = ((NameUseNode*)item->target)->namesym;
    INode *entry = namespaceFind(&((StructNode*)srcdcl)->namespace, srcname);
    if (entry && entry->tag == AliasDclTag)
        return structFoldPath((StructNode*)srcdcl, srcname, path);
    return path;
}

// Hook the entries a trait's expansion added to the namespace, so that a method
// body resolved afterwards can name an inherited member bare. What the
// namespace binds for the name is what is hooked: a name the type already
// declared keeps its own binding, and the collision was reported.
static void structHookInherited(StructNode *node, uint32_t fldpos, uint32_t fldcnt, uint32_t methpos) {
    uint32_t cnt;
    for (cnt = 0; cnt < fldcnt; ++cnt) {
        Name *name = ((FieldDclNode*)nodelistGet(&node->fields, fldpos + cnt))->namesym;
        if (name != anonName)
            nametblHookNode(name, namespaceFind(&node->namespace, name));
    }
    for (cnt = methpos; cnt < node->nodelist.used; ++cnt) {
        FnDclNode *meth = (FnDclNode*)nodelistGet(&node->nodelist, cnt);
        nametblHookNode(meth->namesym, namespaceFind(&node->namespace, meth->namesym));
        if (meth->overloadsym)
            nametblHookNode(meth->overloadsym, namespaceFind(&node->namespace, meth->overloadsym));
    }
}

// Name resolution of a struct type
//
// The dictionary is built whole here, before any method body is resolved: the
// members declared in the type, every default method of each abstraction it
// is-a or mixes in, and, for a variant, its enum's fields. A method body may then
// name an inherited member bare, exactly as it names the type's own. That needs
// each such base resolved first, so it is demanded (structNameResDemand); the
// members copied in
// arrive bound and are not walked again, since name resolution cannot be
// repeated on a node.
//
// A trait that is not yet a declaration -- an instance of a generic trait,
// which exists only once type check instantiates the call -- is left for type
// check to expand by the same steps. Its members cannot be named bare.
// Does every value of this enum consist of nothing but its discriminant?
//
// That is the payload-free form -- a plain set of named symbols, where each
// variant is an empty struct and a common field would be part of every value.
//
// A variant's own fields are counted past the discriminant and past the
// placeholder standing for the enum's fields, because whether the splice has
// happened yet depends on which of the two was reached first: a variant is bound
// in the module ahead of the enum that declares it, and resolving the variant is
// what demands the enum.
static int structEnumIsTagOnly(StructNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        if (!((*nodesp)->flags & IsTagField))
            return 0;
    }
    if (node->derived == NULL)
        return 0;
    for (nodesFor(node->derived, cnt, nodesp)) {
        INode **fldp;
        uint32_t fldcnt;
        for (nodelistFor(&((StructNode*)*nodesp)->fields, fldcnt, fldp)) {
            if (!((*fldp)->flags & (IsMixin | IsTagField)))
                return 0;
        }
    }
    return 1;
}

// Give an enum its equivalence comparison.
//
// An enum compares for equivalence only, never for order: which variant a value
// holds is what a comparison can answer, and the declaration order of variants is
// not a magnitude. Reading the discriminant answers it whole for the payload-free
// form, where the value IS the tag.
//
// Where a variant carries fields, comparing two values would have to compare
// those fields, and Cone has no structural comparison for a struct of any kind.
// So '==' is declared there too and refused when it is called, which is what
// tells the author to use 'match' instead of leaving them to read '=='s absence
// as an oversight.
//
// Entered in the namespace and not in 'nodelist', because this is the enum's own
// comparison and not a requirement on its variants: a vtable slot, a conformance
// requirement and a default cloned into every variant are all read off nodelist.
static void structEnumAddEquality(StructNode *node) {
    if (namespaceFind(&node->namespace, eqName))
        return;
    int tagonly = structEnumIsTagOnly(node);
    FnSigNode *cmpsig = newFnSigNode();
    cmpsig->rettype = (INode*)boolType;
    nodesAdd(&cmpsig->parms, (INode*)newVarDclFull(selfName, VarDclTag, (INode*)node, newPermUseNode(immPerm), NULL));
    nodesAdd(&cmpsig->parms, (INode*)newVarDclFull(anonName, VarDclTag, (INode*)node, newPermUseNode(immPerm), NULL));
    FnDclNode *eqfn = newFnDclNode(eqName, FlagMethFld | FlagPub, (INode*)cmpsig,
        (INode*)newIntrinsicNode(tagonly ? TagEqIntrinsic : NoEqIntrinsic));
    FnDclNode *nefn = newFnDclNode(neName, FlagMethFld | FlagPub, (INode*)cmpsig,
        (INode*)newIntrinsicNode(tagonly ? TagNeIntrinsic : NoEqIntrinsic));
    inodeLexCopy((INode*)eqfn, (INode*)node);
    inodeLexCopy((INode*)nefn, (INode*)node);
    namespaceAdd(&node->namespace, eqName, (INode*)eqfn);
    namespaceAdd(&node->namespace, neName, (INode*)nefn);
}

void structNameRes(NameResState *pstate, StructNode *node) {
    INode **nodesp;
    uint32_t cnt;

    // Reached once: by demand from a type that is-a or mixes it in, or by
    // the module's walk, whichever comes first
    if (node->flags & (NameResolved | NameResolving))
        return;
    node->flags |= NameResolving;
    if (node->flags & EnumType)
        structEnumAddEquality(node);

    INode *svtypenode = pstate->typenode;
    pstate->typenode = (INode*)node;
    nametblHookPush();
    // Resolve generic parameters inside the hooked context. Resolving one hooks
    // it, so doing it before the push would bind it in the enclosing scope and
    // the matching pop would never remove it.
    if (node->genericinfo) {
        for (nodesFor(node->genericinfo->parms, cnt, nodesp))
            inodeNameRes(pstate, nodesp);
    }

    // 'Self' first: a field's type may name it, and a type resolved by demand
    // below hooks its own 'Self' over this one for the duration
    namespaceAdd(&node->namespace, selfTypeName, (INode*)node);
    nametblHookNode(selfTypeName, (INode*)node);

    // Resolve the base before any other name in the type is hooked, and when it is
    // a declaration this type may stand on, stand a placeholder field for it at
    // position 0, as an explicit 'mixin' stands for its trait. The walk below
    // replaces each placeholder with what the base contributes -- an enum's fields
    // for a variant, and for a trait nothing but its default methods, so there the
    // placeholder is simply removed. Anything else about the base -- an instance of
    // a generic, a base that is not a trait, an enum named from outside -- is type
    // check's.
    if (node->basetrait) {
        inodeNameRes(pstate, &node->basetrait);
        StructNode *trait = structNameResTrait(node->basetrait);
        if (trait && (node->flags & HasTagField) == (trait->flags & HasTagField)) {
            FieldDclNode *mixin = newFieldDclNode(trait->namesym, (INode*)immPerm);
            inodeLexCopy((INode*)mixin, node->basetrait);
            mixin->flags |= IsMixin;
            mixin->vtype = node->basetrait;
            nodelistInsert(&node->fields, 0, (INode*)mixin);
        }
    }

    // Each trait to be mixed in, and the type of each field that folds names
    // in, is resolved before this type's own names are hooked, so that its
    // bodies bind in its own scope rather than this type's. Two types that
    // each extend or mix in the other can never both be first; a fold from a
    // type still under way is refused when the clause is expanded below.
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (field->flags & IsMixin) {
            inodeNameRes(pstate, (INode**)nodesp);
            StructNode *trait = structNameResTrait(field->vtype);
            if (trait && !structNameResDemand(pstate, trait))
                errorMsgNode(field->vtype, ErrorCircular,
                    "Cannot extend or mix in %s here: %s is not complete until %s is, so each depends on the other.",
                    &trait->namesym->namestr, &trait->namesym->namestr, &node->namesym->namestr);
        }
        else if (field->fold) {
            inodeNameRes(pstate, &field->vtype);
            INode *srcdcl = structFoldSourceDcl(field);
            if (srcdcl && srcdcl->tag == StructTag)
                structNameResDemand(pstate, (StructNode*)srcdcl);
        }
    }

    // Now hook names inside the type
    nametblHookNamespace(&node->namespace);

    // The methods declared here, to be resolved below once every inherited
    // name is in the dictionary. What the walk splices in after this count
    // arrives already resolved.
    uint32_t ownmethods = node->nodelist.used;

    // Walk the fields backwards, so that replacing a placeholder with the
    // trait's fields does not move a field not yet reached
    int32_t fldpos;
    for (fldpos = node->fields.used - 1; fldpos >= 0; --fldpos) {
        INode **fldnodesp = &nodelistGet(&node->fields, fldpos);
        FieldDclNode *field = (FieldDclNode*)*fldnodesp;
        if (field->fold) {
            // The type was resolved above; the rest of the field here
            inodeNameRes(pstate, (INode**)&field->perm);
            if (field->value)
                inodeNameRes(pstate, &field->value);
            continue;
        }
        if (!(field->flags & IsMixin)) {
            inodeNameRes(pstate, fldnodesp);
            continue;
        }
        StructNode *trait = structNameResTrait(field->vtype);
        if (trait == NULL || !(trait->flags & NameResolved))
            continue;   // type check's to expand, or to refuse
        uint32_t methpos = node->nodelist.used;
        CloneState cstate;
        clonePushState(&cstate, (INode*)node, (INode*)node, 0, NULL, NULL);
        structInheritTrait(node, fldpos, trait, &cstate);
        clonePopState();
        structHookInherited(node, fldpos, structBaseGivesFields(trait) ? trait->fields.used : 0, methpos);
    }

    // Every field now has its place, and a copy a fold makes below takes the
    // index of the field it stands for, so the fields are indexed here. Type
    // check indexes them again after any trait it splices in for an instance.
    uint16_t index = 0;
    for (nodelistFor(&node->fields, cnt, nodesp))
        ((FieldDclNode*)*nodesp)->index = index++;

    // Each fold clause, in field order, after every trait's members are in
    // place: a folded name colliding with an inherited one is reported at the
    // fold. A clause whose field type is not a declaration yet waits for the
    // instance's type check.
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (field->fold)
            structFoldExpand(node, field, 1);
    }

    for (cnt = 0; cnt < ownmethods; ++cnt) {
        inodeNameRes(pstate, &nodelistGet(&node->nodelist, cnt));
    }
    nametblHookPop();
    pstate->typenode = svtypenode;
    node->flags = (node->flags & ~NameResolving) | NameResolved;
}

// Unwrap one hop: the declaration of the base this type names.
// 'basetrait' is written as a name use, or as a generic instantiation call.
// Not the same as structGetBaseTrait below, which recurses to the bottom-most.
StructNode *structBaseTraitDcl(StructNode *node) {
    INode *trait = node->basetrait;
    if (trait == NULL)
        return NULL;
    // Handle if trait is a genericized type
    if (trait->tag == FnCallTag)
        trait = ((FnCallNode*)trait)->objfn;
    return (StructNode*)itypeGetTypeDcl(trait);
}

// Get bottom-most base trait for some trait/struct, or NULL if there is not one
StructNode *structGetBaseTrait(StructNode *node) {
    StructNode *base = structBaseTraitDcl(node);
    if (base == NULL)
        return (node->flags & TraitType) ? node : NULL;
    return structGetBaseTrait(base);
}

// Type check when a type specifies a base trait that has a closed number of variants
void structTypeCheckBaseTrait(StructNode *node) {
    // Get bottom-most base trait
    StructNode *basetrait = structGetBaseTrait(node);

    // We only need special handling when base trait has a closed number of variants
    // i.e., it uses a tag or is fixed-size
    uint16_t isClosedFlags = basetrait->flags & (SameSize | HasTagField);
    if (isClosedFlags == 0)
        return;
    node->flags |= isClosedFlags;  // mark this derived type as having these closed properties

    // A derived type of a closed trait must be declared in the same module
    if (dclInfoGetModule((INode*)basetrait) != dclInfoGetModule((INode*)node)) {
        errorMsgNode((INode*)node, ErrorInvType, "This type must be declared in the same module as the trait");
        return;
    }
}

// Verify that this type meets every method requirement of the traits mixed
// into it. A default the type did not declare was cloned in at expansion and
// meets its requirement by construction. What is left is a name the type
// declares itself, which must have the one candidate of the trait's signature,
// and a requirement with no default, which a struct must implement and a trait
// may pass on to its own implementers. Run after the methods are type checked,
// so that the signatures compared have their types.
static void structCheckTraitReqs(StructNode *node) {
    if (node->traits == NULL)
        return;
    INode **traitp;
    uint32_t traitcnt;
    for (nodesFor(node->traits, traitcnt, traitp)) {
        StructNode *trait = (StructNode*)*traitp;
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&trait->nodelist, cnt, nodesp)) {
            if ((*nodesp)->tag != FnDclTag || !((*nodesp)->flags & FlagMethFld))
                continue;
            FnDclNode *traitmeth = (FnDclNode*)*nodesp;
            INode *binding = namespaceFind(&node->namespace, traitmeth->namesym);
            if (!(node->flags & TraitType) && binding && binding->tag == FnDclTag
                && ((FnDclNode*)binding)->value == NULL) {
                errorMsgNode((INode*)node, ErrorInvType, "Type must implement %s method, as required by %s",
                    &traitmeth->namesym->namestr, &trait->namesym->namestr);
                continue;
            }
            // A trait method is one named requirement. The type satisfies it with
            // a directly named method or the one overload candidate of that signature.
            if (iNsTypeFindVrefMethod(binding, traitmeth) == NULL)
                errorMsgNode((INode*)node, ErrorInvType,
                    "Type declares %s, but none of what it declares has the signature %s requires",
                    &traitmeth->namesym->namestr, &trait->namesym->namestr);
        }
    }
}

// Does this trait require any field of the type that declares 'is' against it?
// A placeholder standing for another abstraction is not a field and stands for
// none, since a trait contributes nothing to a layout.
static int structTraitRequiresFields(StructNode *trait) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&trait->fields, cnt, nodesp)) {
        if (!((*nodesp)->flags & IsMixin))
            return 1;
    }
    return 0;
}

// Verify that this type declares the fields the abstractions it is-a require, and
// that no more than one of them requires any.
//
// A trait contributes no fields. What its fields state is a requirement, which the
// type satisfies by declaring those fields itself -- the same names, the same
// types, in the trait's order, beginning at position 0. That positional prefix is
// what makes a plain reference to the trait a view of the type's own storage
// (structMatches under Regref) and a same-size coercion to it a pure recast, so it
// is a layout requirement and not merely a naming one.
//
// The requirement is the base's OWN field list, which is all a hop has to compare:
// a trait that is-a another trait complied with it here in the same way, so its
// list already carries whatever it was required to declare, and the prefix holds
// at every hop by induction. That is also what lets a trait's default method name
// the fields it needs -- they are the trait's own members.
//
// This is where a field requirement differs from a method requirement, and the
// reason is that a field has no bodiless form: a method may be declared without an
// implementation and so be passed on unimplemented, while declaring a field is
// itself the compliance. So a trait is held to it exactly as a struct is.
//
// Only the FIRST abstraction named may require fields, since only one of them can
// hold position zero. That is what dissolves any contest between two traits' field
// orders: a field two of them want is declared once, and the rest require none.
//
// An enum is exempt, in the other direction: it owns its variants' layout and
// splices its fields in, so a variant declares none of them.
static void structCheckIsaFields(StructNode *node) {
    StructNode *base = structBaseTraitDcl(node);
    if (base != NULL && (base->tag != StructTag || !(base->flags & TraitType)))
        base = NULL;

    // Every abstraction past the first must require no fields at all
    if (node->traits) {
        INode **traitp;
        uint32_t traitcnt;
        for (nodesFor(node->traits, traitcnt, traitp)) {
            StructNode *trait = (StructNode*)*traitp;
            if (trait == base || structBaseGivesFields(trait))
                continue;
            if (structTraitRequiresFields(trait))
                errorMsgNode((INode*)node, ErrorIsaMulti,
                    "%s requires fields, and only the first abstraction named may: its fields would have to hold position zero too",
                    &trait->namesym->namestr);
        }
    }

    if (base == NULL || structBaseGivesFields(base))
        return;

    INode **reqp;
    uint32_t reqcnt;
    uint32_t pos = 0;
    for (nodelistFor(&base->fields, reqcnt, reqp)) {
        if ((*reqp)->flags & IsMixin)
            continue;
        FieldDclNode *req = (FieldDclNode*)*reqp;
        if (pos >= node->fields.used) {
            errorMsgNode((INode*)node, ErrorIsaFields,
                "%s requires a field %s at position %d, which this type does not declare",
                &base->namesym->namestr, &req->namesym->namestr, (int)pos);
            return;
        }
        FieldDclNode *fld = (FieldDclNode*)nodelistGet(&node->fields, pos);
        if (fld->namesym != req->namesym) {
            errorMsgNode((INode*)fld, ErrorIsaFields,
                "%s requires the field %s at position %d. A trait's fields are declared here, in its order, at position 0",
                &base->namesym->namestr, &req->namesym->namestr, (int)pos);
            return;
        }
        if (!itypeIsSame(fld->vtype, req->vtype))
            errorMsgNode((INode*)fld, ErrorIsaFields,
                "Field %s is not the type %s requires of it",
                &fld->namesym->namestr, &base->namesym->namestr);
        ++pos;
    }
}

void structSetDropFn(StructNode *node) {
    // Obtain for struct's `final` method, if any, and validate it
    INode *dropfn = namespaceFind(&node->namespace, finalName);
    if (dropfn != NULL) {
        if (dropfn->tag != FnDclTag) {
            errorMsgNode(dropfn, ErrorBadMeth, "final can only be defined as a method");
            return;
        }

        // Verify dropfn has one self argument of type &uni struct
        FnDclNode *finalfn = (FnDclNode*)dropfn;
        FnSigNode *fnsig = (FnSigNode*)finalfn->vtype;
        if (fnsig->parms->used != 1) {
            errorMsgNode(dropfn, ErrorBadMeth, "method may only have one parameter of type &uni");
            return;
        }
        RefNode *selftype = (RefNode *)(((VarDclNode*)nodesGet(fnsig->parms, 0))->vtype);
        if (selftype->tag != RefTag || selftype->region != borrowRef || itypeGetTypeDcl(selftype->perm) != (INode*)uniPerm) {
            errorMsgNode(dropfn, ErrorBadMeth, "method may only have one parameter of type &uni");
            return;
        }
    }

    // if any field requires drop logic, build a new drop function for struct
    BlockNode *block = NULL;
    INode *selfDcl = NULL;
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        // See whether field's type requires a finalizer
        VarDclNode *fld = (VarDclNode*)*nodesp;
        INode *flddrop = itypeGetDropFnDcl(fld->vtype);
        if (flddrop == NULL)
            continue;

        // Before we can add field's drop logic to new drop function, let's make sure it exists
        if (block == NULL) {
            // Create function dcl for new type drop function
            INode *selftype = newNameUseFromDclNode((INode*)node, (INode*)node);
            INode *refselftype = (INode*)newRefNodeFull(RefTag, (INode*)node, (INode*)borrowRef, (INode*)uniPerm, selftype);
            selfDcl = (INode*)newVarDclFull(selfName, VarDclTag, (INode*)refselftype, (INode*)immPerm, NULL);
            FnSigNode *fnsig = newFnSigNode();
            nodesAdd(&fnsig->parms, (INode*)selfDcl);
            fnsig->rettype = (INode*)newVoidNode();
            block = newBlockNode();
            // Pub, because the value may be dropped wherever it travels: the
            // symbol is reached from any module that holds one of these.
            INode *newdropfn = (INode*)newFnDclNode(dropName, FlagMethFld | FlagPub, (INode*)fnsig, (INode*)block);
            // Owned by the type it drops, so its symbol is spelled after that
            // type and stays unique among all the program's drop functions
            nodelistAdd(&node->nodelist, newdropfn);
            dclInfoJoin(newdropfn, (INode*)node);

            // Block begins with call to struct's finalizer, if there is one
            if (dropfn) {
                FnCallNode *dropfncall = newFnCallLower((INode*)node, dropfn, 1);
                INode *dropnameuse = (INode*)newNameUseFromDclNode(selfDcl, (INode*)node);
                nodesAdd(&dropfncall->args, dropnameuse);
                nodesAdd(&block->stmts, (INode*)dropfncall);
            }
            dropfn = newdropfn;
        }

        // Add call to field's drop fn
        FnCallNode *dropfncall = newFnCallLower((INode*)node, flddrop, 1);
        INode *dropnameuse = (INode*)newNameUseFromDclNode(selfDcl, (INode*)node);
        StarNode *deref = newStarNode(DerefTag);
        deref->vtexp = dropnameuse;
        FnCallNode *fldref = newFnCallLower((INode*)node, (INode*)deref, 0);
        fldref->tag = FldAccessTag;
        fldref->flags |= FlagBorrow;
        fldref->methfld = newNameUseFromDclNode((INode*)fld, (INode*)node);
        nodesAdd(&dropfncall->args, (INode*)fldref);
        nodesAdd(&block->stmts, (INode*)dropfncall);
    }

    // If we have been building a drop function, end it with a return
    if (block != NULL) {
        BreakRetNode *retnode = newReturnNode();
        retnode->exp = (INode*)newNilLitNode();
        retnode->block = block;
        nodesAdd(&block->stmts, (INode*)retnode);
    }

    node->dropfn = dropfn;
}

// Settle the discriminant's width, and refuse a tag value the enum's own integer
// type cannot hold.
//
// The width follows the largest tag VALUE, not the variant count. A pinned value
// is what lines an enum up with an external library's constants, so
// 'Red = 0xFF0000' needs four bytes however few variants there are. An enum that
// declared its integer type has the width fixed there instead, which is the point
// of declaring it, so a value too large for it is the author's error rather than a
// silent widening away from the layout they asked for.
//
// The discriminant's type node is shared rather than cloned (clone.c), so every
// variant's copy of the tag field reads the width set here.
static void structSetTagWidth(StructNode *node) {
    if (node->derived == NULL || !(node->flags & HasTagField))
        return;
    uint32_t maxtag = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->derived, cnt, nodesp)) {
        if (((StructNode*)*nodesp)->tagnbr > maxtag)
            maxtag = ((StructNode*)*nodesp)->tagnbr;
    }
    uint8_t needed = enumBytesFor(maxtag);
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        if (!((*nodesp)->flags & IsTagField))
            continue;
        INode *tagtype = itypeGetTypeDcl(((FieldDclNode*)*nodesp)->vtype);
        if (tagtype->tag != EnumTag)
            continue;
        EnumNode *tagnode = (EnumNode*)tagtype;
        if (tagnode->fixedwidth) {
            if (tagnode->bytes < needed)
                errorMsgNode(tagnode->underlying, ErrorTagWidth,
                    "Tag value %d does not fit in this enum's %d-byte integer type.",
                    (int)maxtag, (int)tagnode->bytes);
        }
        else if (tagnode->bytes < needed)
            tagnode->bytes = needed;
    }
}

// Type check a struct type
void structTypeCheck(TypeCheckState *pstate, StructNode *node) {
    // Wait until a generic struct is instantiated before type checking
    if (node->genericinfo)
        return;

    INode *svtypenode = pstate->typenode;
    pstate->typenode = (INode*)node;

    INode **nodesp;
    uint32_t cnt;

    // Ensure traits keep a list of derived structs/traits
    if ((node->flags & TraitType) && node->derived == NULL)
        node->derived = newNodes(2);

    // Handle when a base trait is specified
    if (node->basetrait) {
        // Name resolution mixed in a base trait that was a declaration then, and
        // recorded it. One that was not -- an instance of a generic trait, which
        // exists only once the instantiation is type checked here -- is mixed in
        // below, through a placeholder field inserted at position 0 as name
        // resolution would have.
        int pending = 1;
        if (node->traits) {
            INode **traitp;
            uint32_t traitcnt;
            for (nodesFor(node->traits, traitcnt, traitp))
                if (isTypeNode(node->basetrait) && *traitp == itypeGetTypeDcl(node->basetrait))
                    pending = 0;
        }
        if (itypeTypeCheck(pstate, &node->basetrait) == 0) {
            pstate->typenode = svtypenode;
            return;
        }
        StructNode *basetrait = (StructNode*)itypeGetTypeDcl(node->basetrait);
        if (basetrait->tag != StructTag || !(basetrait->flags & TraitType)) {
            errorMsgNode(node->basetrait, ErrorInvType, "An 'is' names an abstraction, and this is not one");
        }
        else if ((node->flags & HasTagField) != (basetrait->flags & HasTagField)) {
            // An enum's variants are all declared inside it, so nothing outside
            // can join the set later. An enum extending an enum is not this case:
            // it declares variants of its own, so both sides carry the tag.
            errorMsgNode(node->basetrait, ErrorInvType,
                (basetrait->flags & EnumType)
                    ? "An enum's variants are declared inside it, so nothing outside may join the set"
                    : "A closed set of variants is an enum, and its variants are declared inside it");
        }
        else {
            // Do type-check with bottom-most trait
            // For closed types, the trait and derived node need info from each other
            structTypeCheckBaseTrait(node);

            if (pending) {
                FieldDclNode *mixin = newFieldDclNode(basetrait->namesym, (INode*)immPerm);
                inodeLexCopy((INode*)mixin, node->basetrait);
                mixin->flags |= IsMixin;
                mixin->vtype = node->basetrait;
                nodelistInsert(&node->fields, 0, (INode*)mixin);
            }
        }
    }

    // Every trait name resolution mixed in is type checked before this type's
    // layout is settled, as the one mixed in below is by its placeholder
    if (node->traits) {
        INode **traitp;
        uint32_t traitcnt;
        for (nodesFor(node->traits, traitcnt, traitp))
            inodeTypeCheckAny(pstate, traitp);
    }

    // Iterate backwards through all fields to type check them and to mix in any
    // trait still standing as a placeholder. Backwards, so that replacing a
    // placeholder with the trait's fields does not move a field not yet reached.
    int32_t fldpos;
    for (fldpos = node->fields.used - 1; fldpos >= 0; --fldpos) {
        INode **fldnodesp = &nodelistGet(&node->fields, fldpos);
        FieldDclNode *field = (FieldDclNode*)*fldnodesp;
        if (!(field->flags & IsMixin)) {
            inodeTypeCheckAny(pstate, fldnodesp);
            continue;
        }
        if (itypeTypeCheck(pstate, &field->vtype) == 0) {
            pstate->typenode = svtypenode;
            return;
        }
        StructNode *trait = (StructNode*)itypeGetTypeDcl(field->vtype);
        if (trait->tag != StructTag || !(trait->flags & TraitType)) {
            errorMsgNode(field->vtype, ErrorInvType, "Only an abstraction may be named here, and this is not a trait");
            continue;
        }
        CloneState cstate;
        clonePushState(&cstate, (INode*)node, (INode*)node, 0, NULL, NULL);
        structInheritTrait(node, fldpos, trait, &cstate);
        clonePopState();
    }

    // A fold clause name resolution could not expand -- the field's type was an
    // instance of a generic, which exists only now -- is expanded here, and
    // every folded copy is brought up to date with the field it stands for
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (field->fold && !field->fold->expanded)
            structFoldExpand(node, field, 0);
    }
    structFoldRefresh(pstate, node);

    // Go through all fields to index them and calculate infection flags for ThreadBound/MoveType
    int isZeroSize = 1;  // Start with assumption it is zero size, unless proven otherwise
    int hasEnumFld = 0;
    uint16_t infectFlag = 0;
    uint16_t index = 0;
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        // Number field indexes to reflect their possibly altered position
        ((FieldDclNode*)*nodesp)->index = index++;
        // Notice if a field's threadbound or movetype infects the struct
        ITypeNode *fldtype = (ITypeNode*)itypeGetTypeDcl(((IExpNode*)(*nodesp))->vtype);
        infectFlag |= fldtype->flags & (ThreadBound | MoveType);
        // Handle impact of fields that are opaque or non-zero-size
        if (!itypeIsConcrete((INode*)fldtype))
            node->flags |= OpaqueType;
        if (!itypeIsZeroSize((INode*)fldtype))
            isZeroSize = 0;

        // The discriminant was marked at parse: an enum's first 'tag'-typed
        // field. Any other is a second discriminant, which has no variant number
        // that could mean anything.
        //
        // Asked only of the enum itself. A variant's fields are clones of its
        // enum's, so asking it again would report the enum's mistake once more per
        // variant, at the same position and in the same words -- and a variant
        // writing a discriminant of its own is refused at parse, where only an
        // enum's body accepts one at all.
        if (fldtype->tag == EnumTag && node->basetrait == NULL) {
            if (!((*nodesp)->flags & IsTagField) || hasEnumFld)
                errorMsgNode(*nodesp, ErrorInvType, "A closed type carries one discriminant, and only an enum carries one at all");
            else
                hasEnumFld = 1;
            if (((*nodesp)->flags & IsTagField) && ((FieldDclNode*)(*nodesp))->namesym != anonName)
                errorMsgNode(*nodesp, ErrorInvType, "The discriminant is anonymous: write '_ tag'");
        }
    }
    structSetTagWidth(node);

    // The layout is settled, which is what an 'is' asserts about: the fields the
    // abstractions require are declared here, in order, at position 0
    structCheckIsaFields(node);

    // Use inference rules to decide if struct is ThreadBound or a MoveType
    // based on whether its fields are, and whether it supports the .final or .clone method
    if (namespaceFind(&node->namespace, finalName))
        infectFlag |= MoveType;           // Let's not make copies of finalized objects
    if (namespaceFind(&node->namespace, cloneName))
        infectFlag &= 0xFFFF - MoveType;  // 'clone' means we can make copies anyway

    // Populate infection flags in this struct/trait, and recursively to all inherited traits
    if (infectFlag) {
        node->flags |= infectFlag;
        StructNode *trait = structBaseTraitDcl(node);
        while (trait) {
            trait->flags |= infectFlag;
            trait = structBaseTraitDcl(trait);
        }
    }

    // If there are no fields with non-zero size, we have a zero-size struct
    if (isZeroSize)
        node->flags |= ZeroSizeType;

    // No value may be held of a type whose implementations differ in size: a
    // trait, whose implementers are open-ended, or an '@unsized' enum
    if ((node->flags & TraitType) && !(node->flags & SameSize))
        node->flags |= OpaqueType;

    // Mark the type checked here, before its methods are checked, because this
    // is the point its layout settles: fields are indexed, size is known, and
    // the method set is complete -- mixins were expanded and trait methods
    // inherited during the field walk above. What follows is each method being
    // checked in its own right, which nothing outside this type waits on.
    //
    // The placement is load-bearing, not an optimization. A method may use its
    // own type by value ('fn twin(self) Self'), so the size has to be available
    // before step below runs. See design/phases/type-check.md, "Struct and trait".
    node->flags |= TypeChecked;

    // Type check all methods, etc.
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        inodeTypeCheckAny(pstate, (INode**)nodesp);
    }

    // Now that every method's signature is known, verify that no overload name this
    // type declares has two candidates that would accept the same arguments.
    // Each set is checked once, when its first candidate is reached.
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || ((FnDclNode*)*nodesp)->overloadsym == NULL)
            continue;
        INode *binding = namespaceFind(&node->namespace, ((FnDclNode*)*nodesp)->overloadsym);
        if (binding != NULL && binding->tag == FnOverloadDclTag
            && nodesGet(((FnOverloadDclNode*)binding)->overloads, 0) == *nodesp)
            fnOverloadDclTypeCheck(pstate, (FnOverloadDclNode*)binding);
    }

    structCheckTraitReqs(node);
    structSetDropFn(node);

    pstate->typenode = svtypenode;
}

// Add a vtable implementation to a base struct's vtable
// Return 1 if it successfully type matches, 0 if not
int structAddVtableImpl(StructNode *basenode, StructNode *strnode) {
    Vtable *vtable = basenode->vtable;

    // Create Vtable impl data structure and populate
    VtableImpl *impl = memAllocBlk(sizeof(VtableImpl));
    impl->llvmvtablep = NULL;
    impl->structdcl = (INode*)strnode;

    // For every field/method in the vtable, find its matching one in strnode
    impl->methfld = newNodes(vtable->methfld->used);
    impl->foldpaths = newNodes(vtable->methfld->used);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(vtable->methfld, cnt, nodesp)) {
        if ((*nodesp)->tag == FnDclTag) {
            // Locate the corresponding method with matching name and vtype
            // Note, we need to be flexible in matching the self parameter
            FnDclNode *meth = (FnDclNode *)*nodesp;
            INode *strbinding = namespaceFind(&strnode->namespace, meth->namesym);
            FnDclNode *strmeth = iNsTypeFindVrefMethod(strbinding, meth);
            if (strmeth == NULL)
                return 0;
            // it matches, add the method to the implementation. A method the
            // type holds by folding satisfies the slot too, and the fields its
            // receiver is reached through are recorded for the slot's thunk
            nodesAdd(&impl->methfld, (INode*)strmeth);
            nodesAdd(&impl->foldpaths, strbinding->tag == AliasDclTag
                ? (INode*)structFoldPath(strnode, meth->namesym, NULL) : NULL);
        }
        else {
            // Find the corresponding field with matching name and vtype. A
            // folded copy is a name for storage inside another type's value,
            // not a field of this type, and fills no slot.
            FieldDclNode *fld = (FieldDclNode *)*nodesp;
            INode *strfld = namespaceFind(&strnode->namespace, fld->namesym);
            if (strfld == NULL || strfld->tag != FieldDclTag || ((FieldDclNode*)strfld)->hop) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to a %s virtual reference. Missing field %s.",
                //    &strnode->namesym->namestr, &trait->namesym->namestr, &fld->namesym->namestr);
                return 0;
            }
            TypeCompare match = iexpMatches(&strfld, fld->vtype, Coercion);
            if (match != EqMatch && match != CastSubtype) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to a %s virtual reference. Incompatible type for field %s.",
                //    &strnode->namesym->namestr, &trait->namesym->namestr, &fld->namesym->namestr);
                return 0;
            }
            // it matches, add the corresponding field to the implementation
            nodesAdd(&impl->methfld, (INode*)strfld);
            nodesAdd(&impl->foldpaths, NULL);
        }
    }

    // We accomplished a successful mapping - add it
    nodesAdd(&vtable->impl, (INode*)impl);
    return 1;
}

// Populate the vtable for this struct
void structMakeVtable(StructNode *node) {
    if (node->vtable)
        return;
    Vtable *vtable = memAllocBlk(sizeof(Vtable));
    node->vtable = vtable;
    vtable->trait = (INode*)node;
    vtable->llvmreftype = NULL;
    vtable->llvmvtable = NULL;
    vtable->impl = newNodes(4);

    // Populate methfld with all public methods and then fields in trait
    vtable->methfld = newNodes(node->fields.used + node->nodelist.used);
    uint32_t vtblidx = 0;  // track the index position for each added vtable field
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        // A macro expands where it is used and has nothing to dispatch to
        if ((*nodesp)->tag != FnDclTag)
            continue;
        // Nor has a static function: with no receiver there is nothing to
        // dispatch on, so it is neither a slot nor a requirement an implementer
        // has to satisfy. It stays the declaring type's own.
        if (!((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *meth = (FnDclNode *)*nodesp;
        if (!inodeIsPrivate((INode*)meth)) {
            // A vtable slot holds one machine signature and a generic method has
            // one per instantiation, so there is nothing to put in the slot. The
            // trait's declaration is what is wrong, so that is where this is
            // said, once, when the first virtual reference to it asks for a
            // vtable. The slot is still counted, leaving the requirement one no
            // type can satisfy, so no reference coerces to '&<' this trait.
            if (meth->genericinfo)
                errorMsgNode((INode*)meth, ErrorGenericVtable,
                    "Generic method %s makes %s unusable behind a virtual reference: a vtable slot holds one signature, a generic method one per instantiation.",
                    &meth->namesym->namestr, &node->namesym->namestr);
            meth->vtblidx = vtblidx++;
            nodesAdd(&vtable->methfld, *nodesp);
        }
    }
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode *)*nodesp;
        INode *fieldtyp = itypeGetTypeDcl(field->vtype);
        if (!inodeIsPrivate((INode*)field) && fieldtyp->tag != EnumTag) {
            field->vtblidx = vtblidx++;
            nodesAdd(&vtable->methfld, *nodesp);
        }
    }
    
    // Prewire the implementation of any known derived structs.
    // This is particularly important for keeping tag order correctness
    // when building a virtual ref out of a regular ref to the trait
    // as it uses the tag value to index into a list of vtables
    // Only a trait allocates 'derived'; a plain struct's is NULL
    if (node->derived) {
        for (nodesFor(node->derived, cnt, nodesp)) {
            structAddVtableImpl(node, (StructNode *)*nodesp);
        }
    }
}

// Populate the vtable implementation info for a struct ref being coerced to some trait
TypeCompare structVirtRefMatches(StructNode *trait, StructNode *strnode) {

    if (trait->vtable == NULL)
        structMakeVtable(trait);
    Vtable *vtable = trait->vtable;

    // No need to build VtableImpl for this struct if it has already been done earlier
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(vtable->impl, cnt, nodesp)) {
        VtableImpl *impl = (VtableImpl*)*nodesp;
        if (impl->structdcl == (INode*)strnode)
            return ConvSubtype;
    }

    return structAddVtableImpl(trait, strnode)? ConvSubtype : NoMatch;
}

// Is from-type a subtype of to-struct (we know they are not the same)
// Subtyping is complex on structs for coercions, because we want to avoid the memory management
// messiness of converting struct values from one type to another due to depth/width polymorphism.
//
// The passed constraint establishes the additional subtyping constraints between structs
// - Monomorph accepts any valid subtype, since monomorphization performs no value conversions
// - VirtRef demands that no field require a runtime conversion
// - RegRef demands that supertype's fields appear at start of subtype in order, with no conversion
// - Coercion demands all of the above, plus no added fields
TypeCompare structMatches(StructNode *to, INode *fromdcl, SubtypeConstraint constraint) {
    assert((StructNode*)fromdcl != to);  // We know the types are not equivalent

    // Only a struct may be a subtype of a trait supertype
    if (fromdcl->tag != StructTag || !(to->flags & TraitType))
        return NoMatch;
    StructNode *from = (StructNode*)fromdcl;

    // The virtual reference compare has extra logic for building vtables info
    // So, we just handle it separately
    /*
    if (constraint == Virtref)
        return structVirtRefMatches(to, from);
    */

    // Do the easier check first: Is to-type a base trait of from-type?
    // If so, we know from-type has to-type at its start, making it a great subtype nearly always
    if (to->flags & SameSize) {
        StructNode *super = (StructNode *)fromdcl;
        while (super->basetrait) {
            StructNode *base = (StructNode*)itypeGetTypeDcl(super->basetrait);
            // If it is a valid supertype trait, indicate that it requires coercion
            if (to == base) {
                // With most constraints, a found base trait means we have a valid subtype.
                // However, a non-reference (struct) coercion requires subtype & supertype be the same size
                return constraint == Coercion && !(to->flags & SameSize) ? NoMatch : CastSubtype;
            }
            super = base;
        }
    }
    // If the above test fails, non-ref coercion is not valid
    if (constraint == Coercion)
        return NoMatch;

    // For the Monomorphization & RegRef, we try the slower test
    // of ensuring all methods and fields in supertype are also in subtype
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&to->nodelist, cnt, nodesp)) {
        // Locate the corresponding method with matching name and vtype
        // Note, we need to be flexible in matching the self parameter
        if ((*nodesp)->tag != FnDclTag)
            continue;
        // A static function of the supertype is not a requirement: it has no
        // receiver, so it says nothing about the shape of a value, and it is
        // reached through the type that declares it rather than through this one
        if (!((*nodesp)->flags & FlagMethFld))
            continue;
        FnDclNode *meth = (FnDclNode *)*nodesp;
        INode *frombinding = namespaceFind(&from->namespace, meth->namesym);
        if (iNsTypeFindVrefMethod(frombinding, meth) == NULL)
            return NoMatch;
    }
    // Technique for comparing fields varies ...
    if (constraint == Monomorph) {
        // Monomorphization: Field order is irrelevant. Depth and width subtyping are ok
        for (nodelistFor(&to->fields, cnt, nodesp)) {
            // Find the corresponding field with matching name and vtype
            FieldDclNode *tofld = (FieldDclNode *)*nodesp;
            INode *fromfld = namespaceFind(&from->namespace, tofld->namesym);
            // A folded copy is not a field of the type and meets no requirement
            if (fromfld == NULL || fromfld->tag != FieldDclTag || ((FieldDclNode*)fromfld)->hop) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to %s. Missing field %s.",
                //    &to->namesym->namestr, &to->namesym->namestr, &fld->namesym->namestr);
                return NoMatch;
            }
            TypeCompare match = iexpMatches(&fromfld, tofld->vtype, Monomorph);
            if (match != EqMatch && match != ConvSubtype && match != CastSubtype) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to %s. Incompatible type for field %s.",
                //    &to->namesym->namestr, &to->namesym->namestr, &fld->namesym->namestr);
                return NoMatch;
            }
        }
    }
    else {
        // Regular reference: we need all supertype fields at start of subtype.
        // Width subtyping ok. Depth subtyping only if no field conversions required.
        if (to->fields.used > from->fields.used)
            return NoMatch;
        INode **frmnodesp = &nodelistGet(&from->fields, 0);
        for (nodelistFor(&to->fields, cnt, nodesp)) {
            // Each field of from/to should match (in order) name and vtype
            FieldDclNode *tofld = (FieldDclNode *)*nodesp;
            INode *fromfld = *frmnodesp++;
            if (fromfld == NULL || fromfld->tag != FieldDclTag || ((FieldDclNode*)fromfld)->namesym != tofld->namesym) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to %s. Missing field %s.",
                //    &to->namesym->namestr, &to->namesym->namestr, &fld->namesym->namestr);
                return NoMatch;
            }
            TypeCompare match = iexpMatches(&fromfld, tofld->vtype, Coercion);
            if (match != EqMatch && match != CastSubtype) {
                //errorMsgNode(errnode, ErrorInvType, "%s cannot be coerced to %s. Incompatible type for field %s.",
                //    &to->namesym->namestr, &to->namesym->namestr, &fld->namesym->namestr);
                return NoMatch;
            }
        }
    }

    return CastSubtype;
}

// Return a type that is the supertype of both type nodes, or NULL if none found
INode *structFindSuper(INode *type1, INode *type2) {
    StructNode *typ1 = (StructNode *)itypeGetTypeDcl(type1);
    StructNode *typ2 = (StructNode *)itypeGetTypeDcl(type2);

    // The only supertype supported with structs is they both use the same, same-sized base trait
    if (typ1->basetrait && typ2->basetrait 
        && structGetBaseTrait((StructNode*)itypeGetTypeDcl(typ1->basetrait)) == structGetBaseTrait((StructNode*)itypeGetTypeDcl(typ2->basetrait))
        && (typ1->flags & SameSize))
        return typ1->basetrait;
    // ... or one of them is already the other's base. An
    // inferred type in common meets this the moment a third value arrives: two
    // variants widen the type to their trait, and the third is then being
    // compared against the trait rather than against a sibling. The trait is
    // the answer already reached.
    if (typ2->basetrait && structGetBaseTrait(typ2) == typ1 && (typ2->flags & SameSize))
        return type1;
    if (typ1->basetrait && structGetBaseTrait(typ1) == typ2 && (typ1->flags & SameSize))
        return type2;
    return NULL;
}

// Return a type that is the supertype of both type nodes, or NULL if none found
// This is used by reference types, where same-sized is no longer a requirement
INode *structRefFindSuper(INode *type1, INode *type2) {
    StructNode *typ1 = (StructNode *)itypeGetTypeDcl(type1);
    StructNode *typ2 = (StructNode *)itypeGetTypeDcl(type2);

    // The only supertype supported with structs is they both use the same base trait
    if (typ1->basetrait && typ2->basetrait
        && structGetBaseTrait((StructNode*)itypeGetTypeDcl(typ1->basetrait)) == structGetBaseTrait((StructNode*)itypeGetTypeDcl(typ2->basetrait)))
        return typ1->basetrait;
    // ... or one is already the other's base; see structFindSuper.
    // Size is not a requirement here, because a reference has its own.
    if (typ2->basetrait && structGetBaseTrait(typ2) == typ1)
        return type1;
    if (typ1->basetrait && structGetBaseTrait(typ1) == typ2)
        return type2;
    return NULL;
}
