/** Handling for structs
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include <stdio.h>
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
    snode->extendsbase = NULL;
    snode->extendsdcl = NULL;
    snode->derived = NULL;
    snode->traits = NULL;
    snode->siblings = NULL;
    snode->lifecycle = NULL;
    snode->vtable = NULL;
    snode->genericinfo = NULL;
    snode->tagnbr = 0;
    snode->spans = NULL;
    snode->carriesborrow = CarriesBorrowUnknown;
    return snode;
}

// Clone struct
INode *cloneStructNode(CloneState *cstate, StructNode *node) {
    // An instance of a generic type exists before it is cloned: genericMemoize
    // reserves it, so that a use of the generic's own name anywhere inside can
    // be mapped to it (cloneDclFix) as the clone reaches that use.
    StructNode *newnode = cstate->structshell ? (StructNode*)cstate->structshell : memAllocBlk(sizeof(StructNode));
    cstate->structshell = NULL;
    memcpy(newnode, node, sizeof(StructNode));
    newnode->genericinfo = NULL;
    newnode->lifecycle = NULL;
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    // An instance's fields are the generic's with its parameters bound, so
    // whether they carry a borrow is the instance's own question
    newnode->carriesborrow = CarriesBorrowUnknown;

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
    // An enrichment is taken per instance, in the instance's type check: a
    // template's base is written in terms of its type parameters, so it is a
    // declaration only once they are bound. 'extendsdcl' is therefore NULL in
    // the template and is what the instance's own enrichment sets.
    newnode->extendsbase = cloneNode(cstate, node->extendsbase);
    if (node->derived)
        newnode->derived = newNodes(node->derived->used);
    // The traits name resolution took into the template are the instance's
    // too, since their members are cloned below with everything else. The list
    // is the instance's own, because a generic base trait is taken in only once
    // the instance exists (structTypeCheck) and is appended here.
    if (node->traits) {
        newnode->traits = newNodes(node->traits->used);
        INode **traitp;
        uint32_t traitcnt;
        for (nodesFor(node->traits, traitcnt, traitp))
            nodesAdd(&newnode->traits, *traitp);
    }
    // A sibling 'use' is folded per instance, like an enrichment and for the same
    // reason: the template's clause names a type in terms of its type parameters,
    // so it is a declaration only once they are bound. Each clause is cloned
    // unexpanded (cloneFieldDclNode), so the instance's aliases are its own.
    if (node->siblings)
        newnode->siblings = cloneNodes(cstate, node->siblings);

    // Recreate clones of fields, placeholders and methods, sequentially and in namespace dictionary
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
    //
    // A body may name a static function, a static or an overload name of this
    // type bare, and name resolution bound that use to the original's member.
    // The instance's own copy is what has a symbol; the original's never gets
    // one, and generating a call to it crashed. So each function and static is
    // copied in two steps: first every copy is made and bound in the namespace,
    // and every original member mapped to its copy for cloneDclFix, and only
    // then are the bodies copied -- a body may name a member declared after it.
    // The map is popped with the copy: a later instance maps to its own members.
    uint32_t dclpos = cloneDclPush();
    nodelistInit(&newnode->nodelist, node->nodelist.avail);
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        switch ((*nodesp)->tag) {
        case MacroDclTag:
            iNsTypeAddMacro((INsTypeNode*)newnode, (MacroDclNode*)cloneNode(cstate, *nodesp));
            break;
        case VarDclTag:
            iNsTypeAddStatic((INsTypeNode*)newnode, cloneVarDclShell((VarDclNode*)*nodesp));
            break;
        default:
            iNsTypeAddFn((INsTypeNode*)newnode, cloneFnDclShell((FnDclNode*)*nodesp));
        }
    }
    structCloneMapMembers(node, newnode);
    INode **copyp = newnode->nodelist.nodes;
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag == VarDclTag)
            cloneVarDclFill(cstate, (VarDclNode*)*copyp, (VarDclNode*)*nodesp);
        else if ((*nodesp)->tag != MacroDclTag)
            cloneFnDclFill(cstate, (FnDclNode*)*copyp, (FnDclNode*)*nodesp);
        ++copyp;
    }
    cloneDclPop(dclpos);

    cstate->selftype = svselftype;
    return (INode *)newnode;
}

// Map each function, static and overload name of 'original' to the member of
// 'copy' bound to the same name, so a use of it cloned while the map is in
// force (cloneDclFix) names the copy's. The caller pushes and pops the map.
void structCloneMapMembers(StructNode *original, StructNode *copy) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&original->nodelist, cnt, nodesp)) {
        uint16_t tag = (*nodesp)->tag;
        if (tag != FnDclTag && tag != VarDclTag)
            continue;
        INode *found = namespaceFind(&copy->namespace, inodeGetName(*nodesp));
        if (found && found->tag == tag)
            cloneDclSetMap(*nodesp, found);
        Name *overloadsym = tag == FnDclTag ? ((FnDclNode*)*nodesp)->overloadsym : NULL;
        if (overloadsym) {
            INode *origset = namespaceFind(&original->namespace, overloadsym);
            INode *copyset = namespaceFind(&copy->namespace, overloadsym);
            if (origset && copyset && origset->tag == FnOverloadDclTag && copyset->tag == FnOverloadDclTag)
                cloneDclSetMap(origset, copyset);
        }
    }
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
    INode **nodesp;
    uint32_t cnt;
    // Each sibling a type-body 'use' folds in, by the type it names: the members
    // it admitted are in the namespace and not in any list of this type's own
    if (node->siblings) {
        for (nodesFor(node->siblings, cnt, nodesp)) {
            inodeFprint(" use ");
            inodePrintNode(((FieldDclNode*)*nodesp)->vtype);
        }
    }
    // Each method by name and owner only: an inherited default or a generic
    // instance's method is owned by this type, not by where it was written
    inodeFprint("{");
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

// Expand the placeholder field at 'fldpos' that stands for a base or a further
// name of an 'is' list:
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

// The trait declaration a base or placeholder type expression names, or NULL when it
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
// resolution needs its members: what a type's 'is' list names must have its
// own members in place before they are read. Returns 0 when the type is
// already being resolved, which means the two depend on each other.
//
// Demand is confined to type declarations reached from type declarations, so
// what is hooked at the jump is known: module names, and the demanding type's
// generic parameters. A type declared in another module resolves in that
// module's own scope, with its namespace hooked in place of the current one
// (modHook), so nothing of the demanding module is in reach -- and that
// namespace already holds everything the module folded in, because every
// module's folds run before any module's own resolution does.
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
    // Ordinarily a no-op, since the fold pass has run on every module by now. It
    // is not one when the fold pass itself is what reached this type: a global's
    // fold in one module demanding a struct of another
    modFoldNames(pstate, mod);
    ModuleNode *svmod = pstate->mod;
    pstate->mod = mod;
    modHook(NULL, mod);
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

// Expand a field's fold clause into this type's namespace, hooking each entry
// when name resolution asks. Nothing happens while the field's type is not yet
// a declaration; the clause is then expanded when the instance is type checked.
static void structFoldExpand(StructNode *node, FieldDclNode *field, int hook) {
    FoldClause *fold = field->fold;
    INode *srcdcl = foldSourceDcl(field->vtype);
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
    // An enum before an abstraction, because an enum carries 'TraitType' too
    if (src->flags & EnumType) {
        errorMsgNode(fold->at, ErrorBadFold, "%s is an enum, and its variant set is its identity rather than a set of members to fold.",
            &src->namesym->namestr);
        return;
    }
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
    // Every public member not left out by 'but' -- fields and methods, the
    // source's own folded copies and aliases included, so a fold chains through
    // the types. Not a static, a macro without self, Self, or the value's own
    // finalizer or clone.
    if (fold->star)
        foldStarItems(&src->namespace, src->namesym, fold, FoldAdmitMembers);
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
    INode *srcdcl = foldSourceDcl(field->vtype);
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
    INode *srcdcl = foldSourceDcl(field->vtype);
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
    INode *srcdcl = foldSourceDcl(field->vtype);
    if (srcdcl == NULL || srcdcl->tag != StructTag)
        return path;
    Name *srcname = ((NameUseNode*)item->target)->namesym;
    INode *entry = namespaceFind(&((StructNode*)srcdcl)->namespace, srcname);
    if (entry && entry->tag == AliasDclTag)
        return structFoldPath((StructNode*)srcdcl, srcname, path);
    return path;
}

// ---- 'extends': enriching a concrete type with methods ---------------------
//
// A type declared with 'extends' over a concrete base adds methods and no
// fields. So it has the base's representation exactly, and values of the two
// substitute for each other in both directions at no cost (structExtendsEquiv,
// below). The language is in
// doc/reference/refinherit.html, "Enriching a concrete type".
//
// What it costs the compiler is a NAME FOLD, bar the value's lifecycle. One
// representation means a base method already takes exactly the right receiver,
// so there is no clone to make, no signature to retype and no receiver to shift:
// the base's members become names of this type, and the methods among them are
// reached as the base's own. That is the degenerate, one-instance case of the fold
// a field's 'use' clause does -- there the receiver is the part and has to be
// found, here the receiver is the whole and already is one.
//
// Three things are not aliases. The base's 'final' and 'clone' are CLONED, with
// 'Self' retyped to this type, because they are the value's own lifecycle rather
// than something a call reaches: this type's drop is built from its own 'final'
// (structEnrichLifecycle). The base's FIELDS are copied, because a field node
// carries its index and its own check state and each type lays its own out; the
// copies are this type's declared fields in every respect, so they satisfy an
// 'is' field requirement, fill a vtable slot and are constructed positionally
// exactly as fields written here would be. And what the base's own FOLD CLAUSES
// admitted is left alone, because the clause travels with the field it is
// written on and is expanded again here, into this type's namespace and against
// this type's copy of the field -- which is what keeps a folded name's hop
// pointing at a field of the type that holds it.
//
// The enrichment is INSIDE the base's encapsulation boundary: it is acting as
// the base, which the declaration is what verifies. So a private member comes in
// too, under an alias that is private here as well -- the enrichment's own
// methods read and write the base's private state, while the enrichment's
// clients see only what the base showed them. Down a chain the same rule
// compounds, because what C takes from B includes the private aliases B took
// from A.

// May this declaration be enriched? Report why not, positioned at the clause.
//
// Only a concrete struct can be: an abstraction holds no value, so there is
// nothing of it to enrich and what a type asserts about one is conformance; an
// enum's variant set is its identity, so adding to an enum is adding variants,
// which is a relationship of its own and only another enum may declare it; and a
// variant's fields are its enum's, so it has no representation of its own to stand
// on.
//
// A base declaring 'final' or 'clone' may be enriched: the enrichment gets copies
// of its own of those two (structEnrichLifecycle).
static int structExtendsEligible(StructNode *node, INode *basedcl, INode *at) {
    if (basedcl->tag != StructTag) {
        errorMsgNode(at, ErrorExtendsBase, "An 'extends' enriches a concrete struct type, and this is not one.");
        return 0;
    }
    StructNode *base = (StructNode*)basedcl;
    if (base->flags & EnumType) {
        errorMsgNode(at, ErrorExtendsBase,
            "%s is an enum, and its variant set is its identity: adding to it adds variants, which only another enum may do. Declare %s as an enum.",
            &base->namesym->namestr, &node->namesym->namestr);
        return 0;
    }
    if (base->flags & TraitType) {
        errorMsgNode(at, ErrorExtendsBase,
            "%s is an abstraction, so there is nothing of it to enrich. To assert that %s complies with it, write 'is'.",
            &base->namesym->namestr, &node->namesym->namestr);
        return 0;
    }
    if (base->flags & HasTagField) {
        errorMsgNode(at, ErrorExtendsBase,
            "%s is a variant, and its fields are its enum's: it has no representation of its own to enrich.",
            &base->namesym->namestr);
        return 0;
    }
    return 1;
}

// Is this one of the two methods that belong to a value's own lifecycle? Either
// its own name or the overload name it is a candidate of.
static int structIsLifecycleMeth(INode *meth) {
    if (meth->tag != FnDclTag)
        return 0;
    FnDclNode *fn = (FnDclNode*)meth;
    return fn->namesym == finalName || fn->namesym == cloneName
        || fn->overloadsym == finalName || fn->overloadsym == cloneName;
}

// Give an enrichment its own copies of the base's 'final' and 'clone', with the
// receiver retyped to the enrichment: 'Self' in the copy means this type, exactly
// as it does in a trait's default cloned into an implementer (structInheritTrait).
//
// These two are cloned where every other member is aliased, because they are the
// value's own. structSetDropFn reads 'final' off this type's namespace expecting a
// method whose receiver is this type, and the drop function it generates is this
// type's, called for every value typed as it. An alias there would be read as a
// malformed 'final', and leaving them out would leave the base's finalizer unrun
// for every value typed as the enrichment. So a value finalizes the same way under
// either name, and a 'clone' written against 'Self' produces the enrichment.
//
// What is copied has to be unlowered, because the copy is type checked as this
// type's own method, and a body lowered twice is not the body written: a bare
// method call already given its receiver is given it again. When name resolution
// takes the base's members the base is resolved and not yet type checked, so its
// own methods are copied. When type check takes them -- this type or the base is
// an instance of a generic -- the base has been type checked, and what is copied
// is what it set aside as its layout settled (structKeepLifecycle). Either way the
// body is bound in the base's scope. 'hook' as for structEnrichFromBase.
static void structEnrichLifecycle(StructNode *node, StructNode *base, int hook) {
    INode **nodesp;
    uint32_t cnt;
    if (base->flags & TypeChecked) {
        if (base->lifecycle == NULL)
            return;     // Nothing was set aside, so the base has neither
        nodesp = (INode**)(base->lifecycle + 1);
        cnt = base->lifecycle->used;
    }
    else {
        nodesp = base->nodelist.nodes;
        cnt = base->nodelist.used;
    }
    // What this type declared itself, before any copy joins it
    INode *ownfinal = namespaceFind(&node->namespace, finalName);
    INode *owncl = namespaceFind(&node->namespace, cloneName);
    for (; cnt; cnt--, nodesp++) {
        if (!structIsLifecycleMeth(*nodesp))
            continue;
        // No override, here as for every other name the base has
        Name *name = ((FnDclNode*)*nodesp)->namesym;
        INode *prior = namespaceFind(&node->namespace, name);
        Name *overloadsym = ((FnDclNode*)*nodesp)->overloadsym;
        if (prior == NULL && overloadsym) {
            name = overloadsym;
            prior = overloadsym == finalName ? ownfinal : owncl;
        }
        if (prior) {
            errorMsgNode(prior, ErrorExtendsOverride,
                "%s is already a name of %s, and an enrichment adds to its base rather than overriding it: one value would otherwise mean two things, depending on which name reached it.",
                &name->namestr, &node->namesym->namestr);
            continue;
        }
        CloneState cstate;
        clonePushState(&cstate, (INode*)node, (INode*)node, 0, NULL, NULL);
        FnDclNode *copy = (FnDclNode*)cloneNode(&cstate, *nodesp);
        clonePopState();
        iNsTypeAddFn((INsTypeNode*)node, copy);
        if (hook) {
            nametblHookNode(copy->namesym, namespaceFind(&node->namespace, copy->namesym));
            if (copy->overloadsym)
                nametblHookNode(copy->overloadsym, namespaceFind(&node->namespace, copy->overloadsym));
        }
    }
}

// Set aside unlowered copies of this type's 'final' and 'clone', for an enrichment
// taken after its methods are type checked (structEnrichLifecycle). Called as the
// layout settles, when every copy this type took from its own base is among its
// methods and none of them is lowered yet. 'Self' in them still means this type.
static void structKeepLifecycle(StructNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        if (!structIsLifecycleMeth(*nodesp))
            continue;
        if (node->lifecycle == NULL)
            node->lifecycle = newNodes(2);
        CloneState cstate;
        clonePushState(&cstate, (INode*)node, NULL, 0, NULL, NULL);
        nodesAdd(&node->lifecycle, cloneNode(&cstate, *nodesp));
        clonePopState();
    }
}

// Take everything the base has: its fields as this type's own, and every other
// member as an alias. 'hook' when name resolution asks, so that a method body
// resolved afterwards may name an inherited member bare.
static void structEnrichFromBase(StructNode *node, StructNode *base, int hook) {
    INode **nodesp;
    uint32_t cnt;

    // An enrichment adds methods and no fields: that is what keeps the two
    // representations identical, and the substitution free. Every placeholder is
    // gone by now, so whatever is left in the list was declared here.
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        errorMsgNode(*nodesp, ErrorExtendsField,
            "%s extends %s, so its representation is %s's: its methods read and write the fields it starts with, and it declares none of its own. Remove %s.",
            &node->namesym->namestr, &base->namesym->namestr, &base->namesym->namestr, &field->namesym->namestr);
    }

    // The base's fields, in the base's order, as this type's own
    uint32_t fldpos = 0;
    for (nodelistFor(&base->fields, cnt, nodesp)) {
        FieldDclNode *orig = (FieldDclNode*)*nodesp;
        CloneState cstate;
        clonePushState(&cstate, (INode*)node, (INode*)node, 0, NULL, NULL);
        FieldDclNode *copy = (FieldDclNode*)cloneNode(&cstate, (INode*)orig);
        clonePopState();
        nodelistInsert(&node->fields, fldpos++, (INode*)copy);
        INode *prior = namespaceAdd(&node->namespace, copy->namesym, (INode*)copy);
        if (prior)
            errorMsgNode(prior, ErrorExtendsOverride,
                "%s is already a name of %s, and %s declares a field of that name.",
                &copy->namesym->namestr, &node->namesym->namestr, &base->namesym->namestr);
        else if (hook && copy->namesym != anonName)
            nametblHookNode(copy->namesym, (INode*)copy);
    }

    // Every other member, as an alias: the name is this type's, the declaration
    // stays the base's, and a call through it needs no receiver shift
    namespaceFor(&base->namespace) {
        NameNode *nn = &base->namespace.namenodes[__i];
        if (nn->name == NULL || nn->name == selfTypeName || nn->name == anonName)
            continue;
        // A declared field was copied above; a folded copy and a folded method
        // are remade by the clause that admitted them, which came across with
        // the field it is written on
        FieldDclNode *foldfld;
        if (nn->node->tag == FieldDclTag || structFoldItemOf(base, nn->name, &foldfld))
            continue;
        // The value's own lifecycle is cloned below rather than aliased
        if (nn->name == finalName || nn->name == cloneName || structIsLifecycleMeth(nn->node))
            continue;
        NameUseNode *target = newMemberUseNode(nn->name);
        inodeLexCopy((INode*)target, node->extendsbase);
        target->dclnode = nn->node;
        AliasDclNode *alias = newAliasDclNode(nn->name, (INode*)target);
        inodeLexCopy((INode*)alias, node->extendsbase);
        // Visibility is the base's: the enrichment is inside the boundary and
        // its clients are not
        if (inodeIsPrivate(nn->node))
            alias->flags &= 0xffff - FlagPub;
        if (!inodeIsMember(nn->node))
            alias->flags &= 0xffff - FlagMethFld;
        INode *prior = namespaceAdd(&node->namespace, nn->name, (INode*)alias);
        if (prior) {
            errorMsgNode(prior, ErrorExtendsOverride,
                "%s is already a name of %s, and an enrichment adds to its base rather than overriding it: one value would otherwise mean two things, depending on which name reached it.",
                &nn->name->namestr, &node->namesym->namestr);
            continue;
        }
        if (hook)
            nametblHookNode(nn->name, (INode*)alias);
    }

    structEnrichLifecycle(node, base, hook);

    // What the base's 'is Move' and '@opaque' say is said of the one
    // representation, so a value moves, or may not be held, under either name.
    // What its fields and its 'final' imply is inferred again from the copies
    // above; the declarations are only on the base's flags.
    node->flags |= base->flags & (MoveType | OpaqueType | DeclaredOpaque);

    node->extendsdcl = (INode*)base;
}

// The concrete type at the bottom of this type's 'extends' chain
StructNode *structExtendsRoot(StructNode *node) {
    while (node->extendsdcl)
        node = (StructNode*)node->extendsdcl;
    return node;
}

// ---- 'extends': an enum adding variants to another enum's set --------------
//
// An enum declared with 'extends' over another enum holds COPIES of that enum's
// variants, keeping their tag values, and then its own. So 'RichColors.Red' is a
// variant of RichColors and 'Colors.Red' a variant of Colors: two declarations,
// each in one set, each with a layout of its own. The two enums are TWO DISTINCT
// TYPES that do not substitute for each other in either direction, and a variant
// is a value of the one enum whose set holds it. The language is in
// doc/reference/refenum.html, "Extending an enum"; the mechanism is in
// compiler/c/doc/nodes/struct.md, "An enum extending an enum".
//
// A copy is made the way a generic instance is: the base's variant is resolved
// first, then cloned, so every name inside it stays bound to what it named in the
// base's scope and nothing is resolved again in the wrong module. The copy is then
// made the extension's -- its enum, its owner, its padding -- and bound in the
// extension's namespace. The extension's own fields are the base's, spliced in by
// the placeholder at position 0 exactly as a variant's are, which is what puts the
// base's discriminant and common fields ahead of every added variant's own.
//
// A copy is no module's node. Like a generic instance it is reached through what
// made it: the extension's type check checks it (structTypeCheck), and generation
// reaches it from the extension (genlGlobalSyms, genlGlobalImpl).
//
// 'extendsdcl' stays NULL here, and that is deliberate: it is what
// structExtendsRoot walks and structExtendsEquiv compares, and those answer which
// types substitute for each other. An enum extension licenses no substitution, so
// it writes nothing they read.

// The enum an 'extends' on an enum adds variants to, or NULL for anything else --
// including an enum whose clause was refused, which clears it.
StructNode *structEnumBaseDcl(StructNode *node) {
    if (!(node->flags & EnumType) || node->extendsbase == NULL || !isTypeNode(node->extendsbase))
        return NULL;
    INode *dcl = itypeGetTypeDcl(node->extendsbase);
    return (dcl != NULL && dcl->tag == StructTag && (dcl->flags & EnumType)) ? (StructNode*)dcl : NULL;
}

// How many of this enum's variants are copies of its base's: the first that many
// of its 'derived' list, and 0 for an enum that extends nothing. They are what the
// extension's own passes reach and no module's walk does.
//
// A copy is known by what made it: its 'instnode' is the extension it was cloned
// for (structEnumCopyVariant). That is also what answers 0 for an instance of a
// generic extension, whose list holds instances of the copies: those were made by
// an instantiation, and they are reached through their templates' memonodes, as
// every generic's instances are.
uint32_t structEnumCopyCount(StructNode *node) {
    if (!(node->flags & EnumType) || node->extendsbase == NULL || node->derived == NULL)
        return 0;
    uint32_t copies = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->derived, cnt, nodesp)) {
        if ((*nodesp)->instnode != (INode*)node)
            break;
        ++copies;
    }
    return copies;
}

// Resolve an enum that extends another now, wherever its variants are first
// needed by name: a path 'RichColors.Red', or a module's 'use RichColors;'. Its
// copies of its base's variants exist only once it is resolved.
//
// The need may arise in the middle of a function body, so the enum is resolved
// with nothing of the body in force: no block scope, no enclosing type, and its
// own module's names hooked in place of whatever the body declared. Returns 0 when the
// enum is already being resolved, which means the two depend on each other.
int structEnumDemandSet(NameResState *pstate, StructNode *node) {
    if (!(node->flags & EnumType) || node->extendsbase == NULL || (node->flags & NameResolved))
        return 1;
    if (node->flags & NameResolving)
        return 0;
    NameResState dstate = *pstate;
    dstate.typenode = NULL;
    dstate.loopblock = NULL;
    dstate.macromethod = NULL;
    dstate.scope = 0;
    ModuleNode *mod = dclInfoGetModule((INode*)node);
    int samemod = mod != NULL && mod == pstate->mod;
    if (samemod)
        modHook(NULL, mod);
    int done = structNameResDemand(&dstate, node);
    if (samemod)
        modHook(mod, NULL);
    return done;
}

// May this enum extend the declaration its 'extends' names? Report why not,
// positioned at the clause.
static int structEnumExtendsEligible(StructNode *node, INode *basedcl, INode *at) {
    if (basedcl->tag != StructTag || !(basedcl->flags & EnumType)) {
        errorMsgNode(at, ErrorEnumExtends,
            "An enum extends an enum, adding variants to its set, and this is not an enum.");
        return 0;
    }
    StructNode *base = (StructNode*)basedcl;
    if (base == node) {
        errorMsgNode(at, ErrorEnumExtends, "%s cannot extend itself: its own variants are in its set already.",
            &node->namesym->namestr);
        return 0;
    }
    return 1;
}

// The enum declaration this enum's resolved 'extends' names, or NULL once why not
// is reported. A generic base is named with its arguments, 'Option[i32]' or
// 'Option[T]', and that written instantiation is handed back through 'basecallp'
// for the copies to substitute; it is NULL for a base that is not generic.
//
// The arguments have to be written: the copies are made from the base's variant
// templates with the arguments in place of its parameters, and with none written
// there is nothing to put there. An argument may be one of this enum's own
// parameters, which is how a generic extension passes its parameters on.
static StructNode *structEnumWrittenBase(StructNode *node, FnCallNode **basecallp) {
    INode *written = node->extendsbase;
    *basecallp = NULL;
    FnCallNode *basecall = NULL;
    if (written->tag == FnCallTag) {
        basecall = (FnCallNode*)written;
        written = basecall->objfn;
    }
    if (!isTypeNode(written) || written->tag == FnCallTag) {
        errorMsgNode(node->extendsbase, ErrorEnumExtends,
            "An enum extends an enum, adding variants to its set, and this is not an enum.");
        return NULL;
    }
    INode *basedcl = itypeGetTypeDcl(written);
    if (!structEnumExtendsEligible(node, basedcl, node->extendsbase))
        return NULL;
    StructNode *base = (StructNode*)basedcl;

    uint32_t parmcnt = base->genericinfo ? base->genericinfo->parms->used : 0;
    uint32_t argcnt = basecall && basecall->args ? basecall->args->used : 0;
    if (parmcnt != argcnt) {
        if (argcnt == 0)
            errorMsgNode(node->extendsbase, ErrorArgCount,
                "%s is generic, so what an enum extends is one of its instances, written with its type arguments: %s[...].",
                &base->namesym->namestr, &base->namesym->namestr);
        else if (parmcnt == 0)
            errorMsgNode(node->extendsbase, ErrorArgCount,
                "%s is not generic, so it takes no type arguments.", &base->namesym->namestr);
        else
            errorMsgNode(node->extendsbase, ErrorArgCount,
                "%s has %d type parameters, and %d arguments are written.",
                &base->namesym->namestr, (int)parmcnt, (int)argcnt);
        return NULL;
    }
    if (basecall) {
        int badargs = 0;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(basecall->args, cnt, nodesp)) {
            // A name that did not resolve was reported where it is written
            if (isNameUseNode(*nodesp) && ((NameUseNode*)*nodesp)->dclnode == NULL)
                badargs = 1;
            else if (!isTypeNode(*nodesp) && !nameUseNames(*nodesp, GenVarDclTag)) {
                errorMsgNode(*nodesp, ErrorNotType, "Expected a type for a generic parameter");
                badargs = 1;
            }
        }
        if (badargs)
            return NULL;
    }
    *basecallp = basecall;
    return base;
}

// This enum's copy of one of its base's variants, already resolved.
//
// Cloned as a generic template is cloned into an instance, with 'Self' inside the
// copy meaning the copy (cloneStructNode). Then made the extension's: its enum is
// the extension, which is what its membership, its type check and its layout all
// follow; it answers the requirements the extension inherited; it is padded or
// not as the extension says; and its owner is the extension, so its methods'
// symbols are spelled after the extension and never collide with the base's.
//
// A generic base is written with its arguments ('extends Option[T]'), and what is
// copied is then the base's variant TEMPLATE with the base's parameters replaced
// by those arguments: the substitution generic instantiation performs, made here
// once. 'basecall' is that written instantiation, or NULL for a base that is not
// generic. It is made in two passes, through a stand-in parameter per base
// parameter, because an argument may name one of this enum's own parameters, and
// that parameter may be spelled like the base's -- 'Pending[T] extends Option[T]'.
// Substitution hooks a parameter's NAME to its argument, so in one pass the 'T'
// inside the argument would itself be taken for Option's 'T' and substituted
// again, endlessly. The first pass renames the base's parameters to stand-ins no
// source can spell; the second puts the arguments in their place, each argument
// cloned with nothing of the base hooked.
//
// A generic extension's copy is then a generic template of the extension, as a
// variant the extension declared is (parseAddVariant): parameters of its own,
// spelled as the extension's, and a base written 'Pending[T]'. An instance of the
// extension instantiates it with the rest of its set (genericMemoize). A copy made
// for an extension that is not generic is an ordinary variant: over a generic
// base, the instantiation 'Option[i32]' asks for, made while this enum is
// resolved. Either way the base's fields are spliced into it at type check, as
// they are into a variant of a generic enum, because the base named with
// arguments is an instance, and an instance exists only then. A generic
// extension of a base that is not generic is the exception: the variant it copies
// had its fields spliced at its own name resolution, and the extension's template
// recorded in 'traits' below is what tells structTypeCheck so.
static StructNode *structEnumCopyVariant(StructNode *node, StructNode *base, StructNode *variant, FnCallNode *basecall) {
    CloneState cstate;
    StructNode *copy;
    if (basecall == NULL) {
        clonePushState(&cstate, (INode*)node, NULL, 0, NULL, NULL);
        copy = (StructNode*)cloneNode(&cstate, (INode*)variant);
        clonePopState();
    }
    else {
        Nodes *parms = base->genericinfo->parms;
        Nodes *standins = newNodes(parms->used);
        Nodes *standinuses = newNodes(parms->used);
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(parms, cnt, nodesp)) {
            Name *parmname = ((GenVarDclNode*)*nodesp)->namesym;
            char buf[300];
            size_t len = (size_t)sprintf(buf, "-extends-%s", &parmname->namestr);
            GenVarDclNode *standin = newGVarDclNode(nametblFind(buf, len));
            inodeLexCopy((INode*)standin, *nodesp);
            NameUseNode *use = newNameUseFromLex(standin->namesym, *nodesp);
            use->dclnode = (INode*)standin;
            nodesAdd(&standins, (INode*)standin);
            nodesAdd(&standinuses, (INode*)use);
        }

        clonePushState(&cstate, (INode*)node, NULL, 0, parms, standinuses);
        for (nodesFor(standins, cnt, nodesp))
            nametblHookNode(((GenVarDclNode*)*nodesp)->namesym, *nodesp);
        StructNode *renamed = (StructNode*)cloneNode(&cstate, (INode*)variant);
        clonePopState();

        clonePushState(&cstate, (INode*)node, NULL, 0, standins, basecall->args);
        copy = (StructNode*)cloneNode(&cstate, (INode*)renamed);
        clonePopState();
    }

    if (node->genericinfo) {
        copy->genericinfo = newGenericInfo();
        copy->genericinfo->parms = newNodes(node->genericinfo->parms->used);
        FnCallNode *enumref = newFnCallNode(newNameUseFromDclNode((INode*)node, (INode*)variant),
            node->genericinfo->parms->used);
        inodeLexCopy((INode*)enumref, (INode*)variant);
        enumref->flags |= FlagIndex;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(node->genericinfo->parms, cnt, nodesp)) {
            GenVarDclNode *parm = newGVarDclNode(((GenVarDclNode*)*nodesp)->namesym);
            inodeLexCopy((INode*)parm, *nodesp);
            nodesAdd(&copy->genericinfo->parms, (INode*)parm);
            NameUseNode *parmuse = newNameUseFromLex(parm->namesym, (INode*)variant);
            parmuse->dclnode = (INode*)parm;
            nodesAdd(&enumref->args, (INode*)parmuse);
        }
        copy->basetrait = (INode*)enumref;
    }
    else
        copy->basetrait = newNameUseFromDclNode((INode*)node, (INode*)variant);
    if (copy->traits) {
        INode **traitp;
        uint32_t traitcnt;
        for (nodesFor(copy->traits, traitcnt, traitp)) {
            if (*traitp == (INode*)base)
                *traitp = (INode*)node;
        }
    }
    copy->flags = (copy->flags & ~SameSize) | (node->flags & SameSize);
    dclInfoJoin((INode*)copy, (INode*)node);
    return copy;
}

// Copy the base's variants into this enum's set, ahead of its own, and number its
// own from there.
//
// The copies come FIRST and in the base's order, which is what makes the tag
// values agree in both sets by construction: numbering runs ascending across the
// set, so copying the base's list and continuing from its last value gives every
// copy the value its original has. An added variant may pin a value as any variant
// may, which resets the numbering from there and is refused where it collides with
// a value the set already holds.
//
// Each base variant is resolved first, by demand, so what is copied is complete.
// A chain works by the same demand: the middle enum makes its own copies while it
// is resolved, and they are what the outer one copies.
static void structEnumSeedVariants(NameResState *pstate, StructNode *node, StructNode *base, FnCallNode *basecall) {
    Nodes *own = node->derived;
    uint32_t basecnt = base->derived ? base->derived->used : 0;
    node->derived = newNodes(basecnt + (own ? own->used : 0) + 2);
    INode **nodesp;
    uint32_t cnt;
    uint32_t nexttag = 0;
    if (base->derived) {
        for (nodesFor(base->derived, cnt, nodesp)) {
            StructNode *variant = (StructNode*)*nodesp;
            if (!structNameResDemand(pstate, variant)) {
                errorMsgNode(node->extendsbase, ErrorCircular,
                    "Cannot extend %s here: its variant %s is not complete until %s is, so each depends on the other.",
                    &base->namesym->namestr, &variant->namesym->namestr, &node->namesym->namestr);
                continue;
            }
            StructNode *copy = structEnumCopyVariant(node, base, variant, basecall);
            nodesAdd(&node->derived, (INode*)copy);
            nexttag = copy->tagnbr + 1;
            // One namespace holds an enum's variants, fields and methods. What a copy
            // can meet here is a variant the extension declared under the same name
            // -- or, down a chain, another copy, where the base already reported its
            // own clash. A method or static the extension named after a base variant
            // was reported before the copies were made (structEnumOwnNamesFresh).
            INode *prior = namespaceAdd(&node->namespace, copy->namesym, (INode*)copy);
            if (prior && prior->tag == StructTag && prior->instnode != (INode*)node)
                errorMsgNode(prior, ErrorDupName,
                    "%s is already a variant of %s, copied from %s: an extension holds its base's variants under their own names.",
                    &copy->namesym->namestr, &node->namesym->namestr, &base->namesym->namestr);
        }
    }
    if (own == NULL)
        return;
    for (nodesFor(own, cnt, nodesp)) {
        StructNode *variant = (StructNode*)*nodesp;
        if (variant->tagnbr == TagUnassigned)
            variant->tagnbr = nexttag;
        INode **priorp;
        uint32_t priorcnt;
        for (nodesFor(node->derived, priorcnt, priorp)) {
            if (((StructNode*)*priorp)->tagnbr == variant->tagnbr)
                errorMsgNode((INode*)variant, ErrorDupTag,
                    "Tag value %d is already taken by variant %s.",
                    (int)variant->tagnbr, &((StructNode*)*priorp)->namesym->namestr);
        }
        nodesAdd(&node->derived, *nodesp);
        nexttag = variant->tagnbr + 1;
    }
}

// The enum a resolved enum's 'extends' names, generic or not: the template of a
// base written with its arguments. NULL for an enum that extends nothing.
static StructNode *structEnumWrittenBaseDcl(StructNode *node) {
    if (!(node->flags & EnumType) || node->extendsbase == NULL)
        return NULL;
    INode *written = node->extendsbase;
    if (written->tag == FnCallTag)
        written = ((FnCallNode*)written)->objfn;
    if (!isTypeNode(written) || written->tag == FnCallTag)
        return NULL;
    INode *dcl = itypeGetTypeDcl(written);
    return (dcl != NULL && dcl->tag == StructTag && (dcl->flags & EnumType)) ? (StructNode*)dcl : NULL;
}

// Report a name this extension declares that its base already has, anywhere down
// the chain it extends. An extension adds to what its base declares and neither
// redeclares nor overloads it: the copies of the base's variants already answer
// the name the base's way, so a second declaration would make one value answer it
// two ways, depending on which enum's copy it was. Reported before the copies are
// made, so a copy named like a method here is not reported again.
//
// Down the chain and not only one level, because a generic enum's namespace holds
// what it inherits only per instance: 'Box3[T] extends Box2[T]' reaches Box's
// methods through Box2's instances, never through the template it names.
static void structEnumOwnNamesFresh(StructNode *node, StructNode *base) {
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag && (*nodesp)->tag != VarDclTag)
            continue;
        Name *names[2];
        names[0] = inodeGetName(*nodesp);
        names[1] = (*nodesp)->tag == FnDclTag ? ((FnDclNode*)*nodesp)->overloadsym : NULL;
        int n;
        for (n = 0; n < 2; ++n) {
            if (names[n] == NULL || names[n] == anonName)
                continue;
            StructNode *level;
            for (level = base; level; level = structEnumWrittenBaseDcl(level)) {
                if (namespaceFind(&level->namespace, names[n]) == NULL)
                    continue;
                errorMsgNode(*nodesp, ErrorExtendsOverride,
                    "%s is already a name of %s, which %s extends: an extension adds names to its base's and never redeclares or overloads one, so a name of the base means one thing in every enum that extends it.",
                    &names[n]->namestr, &level->namesym->namestr, &node->namesym->namestr);
                break;
            }
        }
    }
}

// Give each copy of the base's variants the methods this extension declares, cloned
// as an enum's methods are into the variants it declares itself (structInheritTrait):
// resolved, with the copy as 'Self'. The variants it adds get them that way; a copy
// arrives resolved with its enum's members already spliced in, so it is given this
// enum's here, once they are resolved. The base never sees them: they are cloned
// into the extension's copies and not into the variants they were copied from.
//
// A name the copy already answers is left to it, as a variant's own method is left
// to the variant it is declared in: a base's variant may have declared it. A name
// the base's own members have was refused before the copies were made.
//
// Only where the copies were spliced when made: a copy of a generic base's variant
// template is spliced at its own type check, where the extension's methods reach it
// as they reach the variants the extension adds.
static void structEnumCloneOwnMethods(StructNode *node, uint32_t ownmethods) {
    uint32_t copies = structEnumCopyCount(node);
    uint32_t pos;
    for (pos = 0; pos < copies; ++pos) {
        StructNode *copy = (StructNode*)nodesGet(node->derived, pos);
        uint32_t cnt;
        for (cnt = 0; cnt < ownmethods; ++cnt) {
            FnDclNode *meth = (FnDclNode*)nodelistGet(&node->nodelist, cnt);
            if (meth->tag != FnDclTag || !(meth->flags & FlagMethFld) || meth->value == NULL)
                continue;
            if (iNsTypeFindFnField((INsTypeNode*)copy, meth->namesym) != NULL)
                continue;
            CloneState cstate;
            clonePushState(&cstate, (INode*)copy, (INode*)copy, 0, NULL, NULL);
            iNsTypeAddFn((INsTypeNode*)copy, (FnDclNode*)cloneNode(&cstate, (INode*)meth));
            clonePopState();
        }
    }
}

// Is 'name' one that 'ns' or any enum 'enumnode' extends before reaching 'upto'
// binds? The nearer binding is the one in force, so a farther one is not hooked.
static int structEnumNameNearer(Namespace *ns, StructNode *enumnode, StructNode *upto, Name *name) {
    if (namespaceFind(ns, name))
        return 1;
    StructNode *level;
    for (level = structEnumWrittenBaseDcl(enumnode); level && level != upto; level = structEnumWrittenBaseDcl(level)) {
        if (namespaceFind(&level->namespace, name))
            return 1;
    }
    return 0;
}

// Hook, in the current scope, every name the enums 'enumnode' extends declare,
// down its whole chain, that is not already a name of 'ns' -- the extension's own
// namespace -- or of a nearer base, and that is none of 'parms', which are hooked
// in the same scope. So anything written inside an extension's braces sees its
// bases' names bare, exactly as it sees the extension's own, and a name the
// extension declares, or holds as a copy or a clone, wins: it is the nearer one.
// A name is hooked once in a scope, because a scope is unhooked in the order it
// was hooked, and a second hook of one name would be undone into the first.
//
// A base's variants are names the extension holds as copies, and a base's methods
// are ones it holds as clones when the base is not generic. What remains are the
// base's statics and static functions, which are never inherited, and a generic
// base's methods and fields, which reach the extension only per instance, at type
// check. Such a use is bound here to the template's member, and type check points
// it at the instance's (structEnumBaseInstanceMember); a method or field used bare
// is then reached through 'self', by name, as the extension's own are.
static void structEnumHookBaseNames(Namespace *ns, StructNode *enumnode, Nodes *parms) {
    StructNode *level;
    for (level = structEnumWrittenBaseDcl(enumnode); level; level = structEnumWrittenBaseDcl(level)) {
        Namespace *basens = &level->namespace;
        namespaceFor(basens) {
            NameNode *nn = &basens->namenodes[__i];
            if (nn->name == NULL || nn->name == selfTypeName)
                continue;
            if (structEnumNameNearer(ns, enumnode, level, nn->name))
                continue;
            int isparm = 0;
            if (parms) {
                INode **nodesp;
                uint32_t cnt;
                for (nodesFor(parms, cnt, nodesp)) {
                    if (((GenVarDclNode*)*nodesp)->namesym == nn->name)
                        isparm = 1;
                }
            }
            if (!isparm)
                nametblHookNode(nn->name, nn->node);
        }
    }
}

// The member an instance of a generic enum holds for 'dcl', a member of that
// enum's template, when 'where' -- the type whose function is being checked -- is
// an enum that extends that instance, anywhere down its chain, or a variant of
// one; NULL otherwise.
//
// Inside an extension's braces a generic base's names are bare, and name
// resolution bound such a use to the template's member (structEnumHookBaseNames),
// because the instance exists only once the extension's instance is type checked.
// The template's member has no symbol and no self of this set, so the use is
// pointed at the instance's before anything reads it. A method or a field reached
// that way is then lowered to 'self.name', by name, like the extension's own.
INode *structEnumBaseInstanceMember(INode *where, INode *dcl) {
    // An overload name has no owner of its own; its candidates share theirs
    INode *owner = dcl->tag == FnOverloadDclTag
        ? inodeGetOwner(nodesGet(((FnOverloadDclNode*)dcl)->overloads, 0))
        : inodeGetOwner(dcl);
    if (owner == NULL || owner->tag != StructTag || ((StructNode*)owner)->genericinfo == NULL
        || where == NULL || where->tag != StructTag)
        return NULL;
    Nodes *memonodes = ((StructNode*)owner)->genericinfo->memonodes;
    if (memonodes == NULL)
        return NULL;
    StructNode *enumnode = (StructNode*)where;
    if (!(enumnode->flags & EnumType)) {
        enumnode = structBaseTraitDcl(enumnode);
        if (enumnode == NULL || enumnode->tag != StructTag || !(enumnode->flags & EnumType))
            return NULL;
    }
    Name *name = inodeGetName(dcl);
    StructNode *level;
    for (level = structEnumBaseDcl(enumnode); level; level = structEnumBaseDcl(level)) {
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(memonodes, cnt, nodesp)) {
            ++nodesp; --cnt;    // memonodes pairs each instantiating call with its instance
            if (*nodesp != (INode*)level)
                continue;
            INode *member = namespaceFind(&level->namespace, name);
            return member && member->tag == dcl->tag ? member : NULL;
        }
    }
    return NULL;
}

// The declaration a type body's 'use' names, or NULL while it is not one yet --
// an instance of a generic, which exists only at type check
static StructNode *structUseSiblingDcl(FieldDclNode *use) {
    if (use->vtype->tag == FnCallTag || !isTypeNode(use->vtype))
        return NULL;
    INode *dcl = itypeGetTypeDcl(use->vtype);
    return dcl != NULL && dcl->tag == StructTag ? (StructNode*)dcl : NULL;
}

// May this type fold 'sib' in? The shared base is the whole licence: a sibling
// declared the same base, so its methods' receiver is a type this type's values
// substitute for (structExtendsEquiv), and nothing has to be cloned or retyped.
static int structUseSiblingEligible(StructNode *node, StructNode *sib, INode *at) {
    // An enum before an abstraction, because an enum carries 'TraitType' too:
    // it is the closed abstraction over its own variants
    if (sib->flags & EnumType) {
        errorMsgNode(at, ErrorUseSibling,
            "%s is an enum, and its variant set is its identity rather than a set of members to fold.",
            &sib->namesym->namestr);
        return 0;
    }
    if (sib->flags & TraitType) {
        errorMsgNode(at, ErrorUseSibling,
            "%s is an abstraction, and a fold reaches members a value has. To assert that %s complies with it, write 'is'.",
            &sib->namesym->namestr, &node->namesym->namestr);
        return 0;
    }
    if (node->extendsdcl == NULL) {
        errorMsgNode(at, ErrorUseSibling,
            "%s has no base to share, and a sibling is a type that declared the same one. Give %s an 'extends', or delegate instead by declaring a field of %s with its own 'use'.",
            &node->namesym->namestr, &node->namesym->namestr, &sib->namesym->namestr);
        return 0;
    }
    if (sib == node) {
        errorMsgNode(at, ErrorUseSibling, "%s cannot fold itself in.", &node->namesym->namestr);
        return 0;
    }
    if (structExtendsRoot(sib) != structExtendsRoot(node)) {
        errorMsgNode(at, ErrorUseSibling,
            "%s does not share %s's base, so its methods read fields %s does not have. A sibling is a type that declared the same base.",
            &sib->namesym->namestr, &node->namesym->namestr, &node->namesym->namestr);
        return 0;
    }
    // The base of the chain, and every type along it, is reached by 'extends'
    // already: everything it has is here, so a fold of it would collide on every
    // name rather than add one
    StructNode *ancestor = (StructNode*)node->extendsdcl;
    for (;;) {
        if (ancestor == sib) {
            errorMsgNode(at, ErrorUseSibling,
                "%s has every member of %s already, through the 'extends' that starts from it.",
                &node->namesym->namestr, &sib->namesym->namestr);
            return 0;
        }
        if (ancestor->extendsdcl == NULL)
            return 1;
        ancestor = (StructNode*)ancestor->extendsdcl;
    }
}

// Expand one item of a sibling fold: bind its target in the sibling and enter it
// in this type's namespace as an alias.
//
// Always an alias, never a copy, which is what makes this fold the cheap one. The
// sibling's method takes a receiver of the sibling's type, and this type's values
// substitute for it because both declared the same base -- so there is no
// receiver to shift, no signature to retype and no body to clone. A static keeps
// its owner too, which is what a static fold means everywhere.
static void structUseSiblingItem(StructNode *node, StructNode *sib, AliasDclNode *alias, int hook) {
    NameUseNode *target = (NameUseNode*)alias->target;
    Name *srcname = target->namesym;
    INode *found = namespaceFind(&sib->namespace, srcname);
    if (found == NULL) {
        errorMsgNode((INode*)alias, ErrorNoMbr, "%s has no member named %s to fold in.",
            &sib->namesym->namestr, &srcname->namestr);
        return;
    }
    // Ahead of the visibility check, because a lifecycle method does not fold
    // whether it is public or not: every type may have one of its own
    if (srcname == finalName || srcname == cloneName) {
        errorMsgNode((INode*)alias, ErrorBadFold, "%s belongs to %s's own values' lifecycle, so it does not fold.",
            &srcname->namestr, &sib->namesym->namestr);
        return;
    }
    // A sibling is inside the BASE's boundary, not inside this type's: what it
    // declares privately is its own, and only what it shows folds
    if (inodeIsPrivate(found)) {
        errorMsgNode((INode*)alias, ErrorNotPublic, "%s is private to %s, so it does not fold.",
            &srcname->namestr, &sib->namesym->namestr);
        return;
    }
    if (!foldAdmitsOwn(found)) {
        errorMsgNode((INode*)alias, ErrorBadFold,
            "%s comes to %s from the base they share, and %s reaches it by that same route. A sibling fold admits what the sibling adds.",
            &srcname->namestr, &sib->namesym->namestr, &node->namesym->namestr);
        return;
    }
    target->dclnode = found;
    if (!inodeIsMember(found))
        alias->flags &= 0xffff - FlagMethFld;
    INode *prior = namespaceAdd(&node->namespace, alias->namesym, (INode*)alias);
    if (prior) {
        errorMsgNode((INode*)alias, ErrorDupName,
            "%s is already a name of %s. A folded name must be unique: rename it with 'as', or leave it out with 'but'.",
            &alias->namesym->namestr, &node->namesym->namestr);
        return;
    }
    if (hook)
        nametblHookNode(alias->namesym, (INode*)alias);
}

// Expand one type-body 'use' into this type's namespace. Nothing happens while
// the sibling is not a declaration yet; the clause is then expanded when the
// instance is type checked.
static void structUseSiblingExpand(StructNode *node, FieldDclNode *use, int hook) {
    FoldClause *fold = use->fold;
    StructNode *sib = structUseSiblingDcl(use);
    if (sib == NULL)
        return;
    fold->expanded = 1;
    // The sibling's own members must be complete before they are read, and a
    // sibling still under way cannot offer them
    if (sib->tag == StructTag && !(sib->flags & NameResolved) && sib != node) {
        errorMsgNode(fold->at, ErrorCircular, "A fold needs %s complete, and %s is not complete until %s is.",
            &sib->namesym->namestr, &sib->namesym->namestr, &node->namesym->namestr);
        return;
    }
    if (!structUseSiblingEligible(node, sib, fold->at))
        return;
    // An alias for every public member the sibling declares itself, less what
    // 'but' leaves out
    if (fold->star)
        foldStarItems(&sib->namespace, sib->namesym, fold, FoldAdmitOwn);
    INode **itemp;
    uint32_t cnt;
    for (nodesFor(fold->items, cnt, itemp))
        structUseSiblingItem(node, sib, (AliasDclNode*)*itemp, hook);
}

// Expand every type-body 'use' this type carries, in the order written
static void structUseSiblings(StructNode *node, int hook) {
    if (node->siblings == NULL)
        return;
    INode **usep;
    uint32_t cnt;
    for (nodesFor(node->siblings, cnt, usep)) {
        FieldDclNode *use = (FieldDclNode*)*usep;
        if (!use->fold->expanded)
            structUseSiblingExpand(node, use, hook);
    }
}

// Do two types substitute for each other because of an 'extends'?
//
// THE DECLARATION IS THE LICENCE, and identical representation is only the
// argument for why the licence is sound. So this asks whether the declarations
// relate them -- one enriching the other, or both enriching one base, at any
// depth -- and never whether two types happen to look alike. Two look-alikes
// that named no base stay unrelated, and Cone does not get structural typing for
// concrete types out of this.
//
// Nothing lifts through a container either: 'Vec3[Gauge]' and 'Vec3[Meter]' are
// two instances of one template, neither of which extends anything, so they are
// unrelated however their components are declared.
// It answers about two DISTINCT types, which is what an enrichment and its base
// are. One declaration is not substituting for anything, and every caller has
// asked that question already, by identity or through itypeIsSame.
int structExtendsEquiv(INode *type1, INode *type2) {
    INode *dcl1 = itypeGetTypeDcl(type1);
    INode *dcl2 = itypeGetTypeDcl(type2);
    if (dcl1 == dcl2 || dcl1->tag != StructTag || dcl2->tag != StructTag)
        return 0;
    return structExtendsRoot((StructNode*)dcl1) == structExtendsRoot((StructNode*)dcl2);
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
// members declared in the type, every default method of each abstraction its
// 'is' list names, and, for a variant, its enum's fields. A method body may then
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

// The enum a variant is written inside, or NULL for any other type. A variant is
// the one type declared inside another's braces, and its owner says which
// (parseAddVariant). An extension's copy of its base's variant is owned by the
// extension too, but it arrives resolved and is never resolved again.
static StructNode *structEnclosingEnum(StructNode *node) {
    INode *owner = inodeGetOwner((INode*)node);
    if (owner == NULL || owner->tag != StructTag || !(owner->flags & EnumType))
        return NULL;
    return (StructNode*)owner;
}

// The enum whose braces a type is written inside, as privacy counts them: an
// enum is its own, and a variant's is its enum. A variant is asked for the enum
// it belongs to before the one it was written in, so that a variant of a
// generic enum's instance answers the instance, and an extension's copy the
// extension. Any other type is in no enum, and answers NULL.
static StructNode *structPrivacyEnum(INode *type) {
    if (type == NULL || type->tag != StructTag)
        return NULL;
    StructNode *node = (StructNode*)type;
    if (node->flags & EnumType)
        return node;
    StructNode *base = structBaseTraitDcl(node);
    if (base && base->tag == StructTag && (base->flags & EnumType))
        return base;
    return structEnclosingEnum(node);
}

// The enum is the privacy boundary for its variants: code written anywhere
// inside its braces -- its own methods and static functions, and every
// variant's methods -- sees the private members of the enum and of every
// variant, through any value, not only through 'self'. An extension sees what
// its base's code sees, the base's own variants included, and so on down a
// chain; its copies are its own variants. Nothing else changes: a struct's
// privates are still reached only through 'self', a sibling extension's are its
// own, and a base does not see what an extension adds.
//
// The code is the function being checked, and its owner is the type it is
// written in: a method cloned into a variant or a copy is owned by the clone's
// type, and an instance's by the instance. A function owned by a module -- a
// free function, an anonymous one, or the instance of a generic function
// instantiated from inside the enum -- is outside every enum's braces.
int structEnumSeesPrivate(TypeCheckState *pstate, INode *type) {
    if (pstate == NULL || pstate->fn == NULL)
        return 0;
    StructNode *memberenum = structPrivacyEnum(type);
    if (memberenum == NULL)
        return 0;
    StructNode *siteenum = structPrivacyEnum(inodeGetOwner((INode*)pstate->fn));
    while (siteenum) {
        if (siteenum == memberenum)
            return 1;
        siteenum = structEnumBaseDcl(siteenum);
    }
    return 0;
}

// Hook the names of the enum a variant is written inside, beneath the variant's
// own: its variants, its statics, its fields and whatever else it declares --
// except, for an enum that is not generic, the methods a value of it answers. The
// variant has those as its own clones, hooked in the nearer scope as they are
// spliced in. A generic enum's are cloned in only when an instance is type
// checked, so its methods and method overload names are hooked here: a bare use
// binds to the enum's, the instance's clone re-points it at the enum instance's
// (genericMemoize), and type check lowers a bare method call to 'self.name', found
// by name in the variant, whose own clone it is. A field needs no such care
// either way: a bare field is read through self by name, so the enum's own serves.
//
// An extension's bases' names follow in the same frame, each only where the
// extension has no name of its own for it, so a variant the extension adds sees
// them bare as the extension's methods do (structEnumHookBaseNames).
static void structHookEnclosingEnum(StructNode *enumnode) {
    Namespace *ns = &enumnode->namespace;
    namespaceFor(ns) {
        NameNode *nn = &ns->namenodes[__i];
        if (nn->name == NULL)
            continue;
        INode *dcl = nn->node;
        if ((dcl->flags & FlagMethFld)
            && (((dcl->tag == FnDclTag || dcl->tag == FnOverloadDclTag) && !enumnode->genericinfo)
                || dcl->tag == MacroDclTag))
            continue;
        nametblHookNode(nn->name, dcl);
    }
    structEnumHookBaseNames(ns, enumnode, NULL);
}

// Refuse a name written in an 'is' list that is a closed type, and say whether
// it was one. Only a name read from a list is asked: the placeholder that stands
// for a base -- a variant's enum, an extension's base -- carries the base's name,
// while one read from a list is anonymous (parseStruct).
//
// An enum's variants are all declared inside it, so nothing outside may join its
// set, and a type that took one in would carry its discriminant and common
// fields with no variant number that means anything in it. The first name of a
// struct's or a trait's list is its base, refused by type check in the same words;
// this is every other name, and every name of an enum's or a variant's list.
// 'ownenum' is the enum a variant belongs to, whose name the parser refuses
// already where it is spelled bare; here it arrives spelled some other way.
static int structRefuseClosedIs(StructNode *node, FieldDclNode *field, StructNode *trait, StructNode *ownenum) {
    if (field->namesym != anonName || !(trait->flags & EnumType))
        return 0;
    if (ownenum && trait == ownenum)
        errorMsgNode((INode*)node, ErrorVariantDcl,
            "%s is a member of the enum it is written inside; remove the 'is'.",
            &node->namesym->namestr);
    else
        errorMsgNode(field->vtype, ErrorInvType,
            "An enum's variants are declared inside it, so nothing outside may join the set");
    return 1;
}

void structNameRes(NameResState *pstate, StructNode *node) {
    INode **nodesp;
    uint32_t cnt;

    // Reached once: by demand from a type that names it in an 'is' list, or by
    // the module's walk, whichever comes first
    if (node->flags & (NameResolved | NameResolving))
        return;
    node->flags |= NameResolving;
    // An extension's comparison waits until its set is its base's plus its own,
    // below: what the comparison can be is decided by whether any variant in the
    // whole set carries fields.
    if ((node->flags & EnumType) && node->extendsbase == NULL)
        structEnumAddEquality(node);

    INode *svtypenode = pstate->typenode;
    pstate->typenode = (INode*)node;
    // Nothing of an enclosing body is in force in a type's braces: each method
    // decides for itself whether an importer expands it (fnDclNameRes). A
    // field's default is a literal, so it names nothing to mark
    INode *svexpander = pstate->expander;
    pstate->expander = NULL;

    // Anything written inside an enum's braces sees every name the enum declares
    // bare, and a variant's body is written there. The enum's own methods get that
    // from the enum's namespace being hooked while the enum resolves; a variant is
    // a module node resolved on its own, so the enum's namespace is hooked here, in
    // a scope of its own beneath the variant's. The variant's own names -- its
    // fields, its methods, 'Self', its generic parameters, and what it inherits --
    // are hooked in the inner scope, so they win a clash with a name of the enum.
    //
    // The enum is demanded first, because an extension's namespace receives its
    // copies of its base's variants only while it is resolved. It is the enum the
    // variant stands on in any case, which its placeholder below demands anyway; a
    // demand that finds the enum under way leaves it to that one to report.
    StructNode *enclosing = structEnclosingEnum(node);
    if (enclosing) {
        structNameResDemand(pstate, enclosing);
        nametblHookPush();
        structHookEnclosingEnum(enclosing);
    }

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
    // position 0, as a further name of an 'is' list stands for its trait. This
    // one carries the base's name, which is what tells it from those
    // (structRefuseClosedIs): it is the one way an enum's fields reach a type.
    // The walk below
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

    // An enum's 'extends' names the enum whose variants this one copies into its
    // own set, and gives its copies the methods it declares once those are
    // resolved, at the end (structEnumCloneOwnMethods). Resolved and demanded here
    // for the reason a base trait is -- the base's variants and fields have to be complete before they are copied -- and
    // then the base stands as a placeholder at position 0, exactly as a variant's
    // enum does, so that the base's discriminant and common fields are spliced in by
    // the one mechanism. Before the namespace is hooked below, so the copies are
    // names of this enum like the variants it declares.
    //
    // Either side of the clause may be generic. A generic base is named with its
    // arguments, which is an instance, so its fields are spliced in at type check,
    // where an instance exists, as a generic enum's are into its variants: the
    // placeholder then holds its own copy of the written instantiation, since type
    // check replaces the placeholder's type with the instance and 'extendsbase'
    // with it separately. The copies are made here all the same, from the base's
    // variant templates (structEnumCopyVariant).
    int clonecopies = 0;
    if (node->flags & EnumType) {
        if (node->extendsbase) {
            inodeNameRes(pstate, &node->extendsbase);
            FnCallNode *basecall = NULL;
            StructNode *enumbase = structEnumWrittenBase(node, &basecall);
            if (enumbase && !structNameResDemand(pstate, enumbase)) {
                errorMsgNode(node->extendsbase, ErrorCircular,
                    "Cannot extend %s here: %s is not complete until %s is, so each depends on the other.",
                    &enumbase->namesym->namestr, &enumbase->namesym->namestr, &node->namesym->namestr);
                enumbase = NULL;
            }
            if (enumbase == NULL)
                node->extendsbase = NULL;   // Reported; nothing downstream asks again
            else {
                structEnumOwnNamesFresh(node, enumbase);
                structEnumSeedVariants(pstate, node, enumbase, basecall);
                clonecopies = basecall == NULL;
                structEnumAddEquality(node);
                FieldDclNode *mixin = newFieldDclNode(enumbase->namesym, (INode*)immPerm);
                inodeLexCopy((INode*)mixin, node->extendsbase);
                mixin->flags |= IsMixin;
                if (basecall) {
                    CloneState cstate;
                    clonePushState(&cstate, (INode*)node, NULL, 0, NULL, NULL);
                    mixin->vtype = cloneNode(&cstate, node->extendsbase);
                    clonePopState();
                }
                else
                    mixin->vtype = node->extendsbase;
                nodelistInsert(&node->fields, 0, (INode*)mixin);
            }
        }
        // A clause that was refused above leaves an enum standing on its own
        // variants, which is what its comparison is then made from. Idempotent:
        // an enum that never had a clause has it already.
        structEnumAddEquality(node);
    }

    // The concrete base an 'extends' enriches is resolved and demanded here, for
    // the same reason a trait is: its members have to be complete before they are
    // taken. Taking them waits until the field walk below has removed every
    // placeholder, so that whatever is left in the field list is a field this
    // type declared -- which is what 'extends' forbids.
    StructNode *extbase = NULL;
    if (!(node->flags & EnumType) && node->extendsbase) {
        inodeNameRes(pstate, &node->extendsbase);
        if (!(node->genericinfo) && node->extendsbase->tag != FnCallTag && isTypeNode(node->extendsbase)) {
            INode *basedcl = itypeGetTypeDcl(node->extendsbase);
            if (structExtendsEligible(node, basedcl, node->extendsbase)) {
                extbase = (StructNode*)basedcl;
                if (extbase == node) {
                    errorMsgNode(node->extendsbase, ErrorCircular,
                        "%s cannot extend itself: an enrichment starts from a type that is already complete.",
                        &node->namesym->namestr);
                    extbase = NULL;
                }
                else if (!structNameResDemand(pstate, extbase)) {
                    errorMsgNode(node->extendsbase, ErrorCircular,
                        "Cannot extend %s here: %s is not complete until %s is, so each depends on the other.",
                        &extbase->namesym->namestr, &extbase->namesym->namestr, &node->namesym->namestr);
                    extbase = NULL;
                }
            }
        }
    }

    // Each abstraction to be taken in, and the type of each field that folds
    // names in, is resolved before this type's own names are hooked, so that its
    // bodies bind in its own scope rather than this type's. Two types that
    // each stand on the other can never both be first; a fold from a type still
    // under way is refused when the clause is expanded below. A name of an 'is'
    // list that turns out to be a closed type is refused here and its
    // placeholder dropped, so nothing of the enum is spliced in.
    uint32_t fldi = 0;
    while (fldi < node->fields.used) {
        nodesp = &nodelistGet(&node->fields, fldi);
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (field->flags & IsMixin) {
            inodeNameRes(pstate, (INode**)nodesp);
            StructNode *trait = structNameResTrait(field->vtype);
            if (trait && structRefuseClosedIs(node, field, trait, enclosing)) {
                nodelistMakeSpace(&node->fields, fldi, -1);
                continue;
            }
            if (trait && !structNameResDemand(pstate, trait))
                errorMsgNode(field->vtype, ErrorCircular,
                    "Cannot take in %s here: %s is not complete until %s is, so each depends on the other.",
                    &trait->namesym->namestr, &trait->namesym->namestr, &node->namesym->namestr);
        }
        else if (field->fold) {
            inodeNameRes(pstate, &field->vtype);
            INode *srcdcl = foldSourceDcl(field->vtype);
            if (srcdcl && srcdcl->tag == StructTag)
                structNameResDemand(pstate, (StructNode*)srcdcl);
        }
        ++fldi;
    }

    // And every sibling a type-body 'use' names, on the same terms and for the
    // same reason. Its own 'extends' has to have been taken before the base it
    // shares can be compared with this type's, which is what demanding it does.
    if (node->siblings) {
        for (nodesFor(node->siblings, cnt, nodesp)) {
            FieldDclNode *use = (FieldDclNode*)*nodesp;
            inodeNameRes(pstate, &use->vtype);
            StructNode *sib = structUseSiblingDcl(use);
            if (sib && sib != node)
                structNameResDemand(pstate, sib);
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

    // Now that every placeholder is gone, the concrete base's members are taken:
    // its fields become this type's, at the front and in its order, and every
    // other member becomes an alias. Before the indexing below, so the copies are
    // indexed with everything else, and before the fold clauses, so a clause that
    // came across with a copied field is expanded against the copy.
    if (extbase)
        structEnrichFromBase(node, extbase, 1);

    // Then each sibling a type-body 'use' names: after the base, because the
    // shared base is what licenses the fold and this type's own members must be
    // in place for a collision to be reported at the clause that caused it.
    structUseSiblings(node, 1);

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

    // An extension's bases' names are bare inside its braces too, beneath its own.
    // Hooked last, once every name this enum holds -- its copies, and the clones
    // of a base's methods the walk above spliced in -- is in its namespace.
    if (node->flags & EnumType)
        structEnumHookBaseNames(&node->namespace, node, node->genericinfo ? node->genericinfo->parms : NULL);

    for (cnt = 0; cnt < ownmethods; ++cnt) {
        inodeNameRes(pstate, &nodelistGet(&node->nodelist, cnt));
    }
    if (clonecopies)
        structEnumCloneOwnMethods(node, ownmethods);
    // Every abstraction is taken in. A type declaring 'is Move' moves whatever
    // it holds [Jon 26 Sep], marked as soon as that is known, so that a type
    // asking before this one is laid out is answered
    if (structDeclaresTrait(node, moveTrait))
        node->flags |= MoveType;
    nametblHookPop();
    if (enclosing)
        nametblHookPop();
    pstate->typenode = svtypenode;
    pstate->expander = svexpander;
    node->flags = (node->flags & ~NameResolving) | NameResolved;
}

// Does this type's 'is' list name this trait? 'traits' records every
// abstraction the list named, the first included, once name resolution (or,
// for an instance of a generic trait, type check) has taken it in.
int structDeclaresTrait(StructNode *node, StructNode *trait) {
    if (node->traits == NULL)
        return 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->traits, cnt, nodesp)) {
        if (*nodesp == (INode*)trait)
            return 1;
    }
    return 0;
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
            // An 'extern' method has no body here and is implemented all the same:
            // its definition is in the object that defines the type
            if (!(node->flags & TraitType) && binding && binding->tag == FnDclTag
                && ((FnDclNode*)binding)->value == NULL && !(binding->flags & FlagExtern)) {
                errorMsgNode((INode*)node, ErrorInvType, "Type must implement %s method, as required by %s",
                    &traitmeth->namesym->namestr, &trait->namesym->namestr);
                continue;
            }
            // A trait method is one named requirement. The type satisfies it with
            // a directly named method or the one overload candidate of that signature.
            //
            // The requirement's signature is a use too (Rule 1). This type may have
            // been demanded from inside the trait's own check -- a trait method
            // declared above this one names this type in its signature -- so the
            // trait has not reached this method yet, and its unchecked signature
            // matched nothing. It is analyzed here under the trait's walk state
            // (Rule 8). One under way already has its signature (Rule 3).
            if (!(traitmeth->flags & (TypeChecked | TypeChecking))) {
                TypeCheckState tstate;
                tstate.typenode = (INode*)trait;
                tstate.fn = NULL;
                tstate.scope = 0;
                inodeTypeCheckAny(&tstate, nodesp);
                traitmeth = (FnDclNode*)*nodesp;
            }
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

    // Every abstraction past the first must require no fields at all. An enum
    // and a variant have no first: every name in their lists stands beside the
    // enum, which lays out itself and every variant, so none of them may require
    // fields, and the message says why in those terms
    int enumlaid = (node->flags & EnumType) || (base && structBaseGivesFields(base));
    if (node->traits) {
        INode **traitp;
        uint32_t traitcnt;
        for (nodesFor(node->traits, traitcnt, traitp)) {
            StructNode *trait = (StructNode*)*traitp;
            if (trait == base || structBaseGivesFields(trait))
                continue;
            if (!structTraitRequiresFields(trait))
                continue;
            if (enumlaid)
                errorMsgNode((INode*)node, ErrorIsaMulti,
                    "%s requires fields, and an enum's 'is' or a variant's may name only abstractions that require none: the enum lays out itself and every variant",
                    &trait->namesym->namestr);
            else
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

    // A trait's methods -- an enum's too -- belong to its implementers: each is
    // cloned into them or required of them, and none is generated for the trait.
    // A drop built here would be one more, a requirement none could meet, since
    // each one's own drop takes its own self. Each already drops those fields: an
    // enum's common fields are spliced into its variants, and the fields a trait
    // requires are declared by its implementers.
    //
    // Nor is its own 'final' its drop: that too is never generated for it, so a
    // call to it had no function to reach, and the compiler crashed on a value
    // typed as the enum. Such a value is not finalized, whether or not the enum
    // declares a 'final'; dispatching on the tag to the variant's drop is the
    // unbuilt "final handling for union".
    if (node->flags & TraitType) {
        node->dropfn = NULL;
        return;
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
            // Built lowered, so it carries the mark a check would have left, and
            // the walk over this type's members (structCheckMembers) passes it by
            newdropfn->flags |= TypeChecked;
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
//
// An extension reads the width its base settled and may not widen it. The node is
// the base's, so widening it here would relay out the base's own values behind it,
// for the sake of a set the base knows nothing about. A value that does not fit is
// therefore refused where it was added -- which is the same question a declared
// integer type asks, so it wears the same code.
//
// An instance of a generic enum is not measured from its own type check, but by
// genericMemoize once the instance and its variants are all checked, and only for
// the generic's first instance: every instance shares the template's
// discriminant node and tag values, so measuring each would report a declared
// integer type's overflow once per instance (structTypeCheckEnumInstance).
void structSetTagWidth(StructNode *node) {
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
    StructNode *enumbase = structEnumBaseDcl(node);
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
        else if (enumbase) {
            if (tagnode->bytes < needed)
                errorMsgNode((INode*)node, ErrorTagWidth,
                    "Tag value %d does not fit the %d-byte discriminant %s lays its variants out in, which %s shares.",
                    (int)maxtag, (int)tagnode->bytes, &enumbase->namesym->namestr, &node->namesym->namestr);
        }
        else if (tagnode->bytes < needed)
            tagnode->bytes = needed;
    }
}

// The instance of a generic enum being type checked by
// structTypeCheckEnumInstance, whose discriminant genericMemoize measures itself
static StructNode *structTagWidthDeferred = NULL;

// Type check an instance of a generic enum, whose 'derived' already lists its
// variants, leaving its discriminant's width to genericMemoize (structSetTagWidth).
// Saved and restored, as the check may instantiate another generic enum.
void structTypeCheckEnumInstance(TypeCheckState *pstate, StructNode *instance) {
    StructNode *saved = structTagWidthDeferred;
    structTagWidthDeferred = instance;
    INode *node = (INode*)instance;
    inodeTypeCheckAny(pstate, &node);
    structTagWidthDeferred = saved;
}

// -------- Layout before members --------
//
// Type check lays out every type before it checks any type's members. A layout
// is what a size question reads, and what a move or a thread question reads, so a
// member checked while some layout is still in flight could see a type with no
// size yet, or one that does not yet know it moves -- and whether it did would
// follow the order the declarations happen to be written in. So a layout never
// checks members: structTypeCheck lays the type out and puts it in the members
// queue, and the queue is worked only when no layout is in flight.
//
// 'In flight' is counted by structLayoutEnter and structLayoutExit, around the
// type check of every type that holds values by value -- a struct, an array and
// a tuple (inodeTypeCheck). A reference answers its own size and a function
// signature has none, so neither is counted. See
// compiler/c/doc/phases/type-check.md, "Layout before members".
static uint32_t structLayoutDepth = 0;
// Types laid out whose members are not yet checked, first laid out first
static Nodes *structMembersWaiting = NULL;
static uint32_t structMembersNext = 0;
// Enums demanded from inside one of their own variants' layouts, whose other
// variants are laid out once that layout is done
static Nodes *structVariantsWaiting = NULL;
static uint32_t structVariantsNext = 0;

static void structWait(Nodes **queue, StructNode *node) {
    if (*queue == NULL)
        *queue = newNodes(16);
    nodesAdd(queue, (INode*)node);
}

// Is this a closed set -- an enum, or an instance of one -- whose variants its
// size and its move and thread properties come from?
static int structIsClosedSet(StructNode *node) {
    return (node->flags & TraitType) && (node->flags & (HasTagField | SameSize))
        && node->derived != NULL;
}

// Is one of this closed set's variants being laid out right now?
static int structVariantInFlight(StructNode *node) {
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->derived, cnt, nodesp)) {
        if (((*nodesp)->flags & TypeChecking) && !((*nodesp)->flags & TypeChecked))
            return 1;
    }
    return 0;
}

// Lay out each variant of a closed set not laid out already. An extension's
// copies of its base's variants are among them, at the front of the list.
static void structLayoutVariants(TypeCheckState *pstate, StructNode *node) {
    uint32_t pos;
    for (pos = 0; pos < node->derived->used; ++pos)
        inodeTypeCheckAny(pstate, &nodesGet(node->derived, pos));
}

// 'is Copy' is an assertion [Jon 26 Sep]: the type is refused where it moves
// after all. Asked once every layout has finished, since an enum moves when
// one of its variants does, and the variants are laid out after the enum.
static void structCheckCopy(StructNode *node) {
    if (!structDeclaresTrait(node, copyTrait) || !(node->flags & MoveType))
        return;
    char *name = &node->namesym->namestr;
    if (structDeclaresTrait(node, moveTrait)) {
        errorMsgNode((INode*)node, ErrorCopyMove,
            "%s declares both Move and Copy, and a type is exactly one of them. Keep the one it is.", name);
        return;
    }
    if (namespaceFind(&node->namespace, finalName)) {
        errorMsgNode((INode*)node, ErrorCopyMove,
            "%s is declared Copy, but its 'final' makes it move: each copy would be finalized.", name);
        return;
    }
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->fields, cnt, nodesp)) {
        FieldDclNode *field = (FieldDclNode*)*nodesp;
        if (itypeIsMove(field->vtype)) {
            errorMsgNode((INode*)node, ErrorCopyMove,
                "%s is declared Copy, but its field '%s' moves, and so it moves too.",
                name, &field->namesym->namestr);
            return;
        }
    }
    if (node->extendsdcl && itypeIsMove(node->extendsdcl)) {
        errorMsgNode((INode*)node, ErrorCopyMove,
            "%s is declared Copy, but the base it enriches, %s, moves, and so it moves too.",
            name, &((StructNode*)node->extendsdcl)->namesym->namestr);
        return;
    }
    errorMsgNode((INode*)node, ErrorCopyMove,
        (node->flags & EnumType) ? "%s is declared Copy, but one of its variants moves, and so it moves too."
            : "%s is declared Copy, but it moves.", name);
}

// Check a laid-out type's members: its methods, static functions and statics,
// then every overload set it declares, then what the traits taken into it require
// of those members. Every layout the program has begun is finished by now.
static void structCheckMembers(StructNode *node) {
    TypeCheckState tstate;
    tstate.typenode = (INode*)node;
    tstate.fn = NULL;
    tstate.scope = 0;

    // A generated drop fn carries its mark already, and is passed by
    uint32_t methcnt = node->nodelist.used;
    uint32_t pos;
    for (pos = 0; pos < methcnt; ++pos)
        inodeTypeCheckAny(&tstate, &nodelistGet(&node->nodelist, pos));

    // Now that every method's signature is known, verify that no overload name this
    // type declares has two candidates that would accept the same arguments.
    // Each set is checked once, when its first candidate is reached.
    INode **nodesp;
    uint32_t cnt;
    for (nodelistFor(&node->nodelist, cnt, nodesp)) {
        if ((*nodesp)->tag != FnDclTag || ((FnDclNode*)*nodesp)->overloadsym == NULL)
            continue;
        INode *binding = namespaceFind(&node->namespace, ((FnDclNode*)*nodesp)->overloadsym);
        if (binding != NULL && binding->tag == FnOverloadDclTag
            && nodesGet(((FnOverloadDclNode*)binding)->overloads, 0) == *nodesp)
            fnOverloadDclTypeCheck(&tstate, (FnOverloadDclNode*)binding);
    }

    structCheckTraitReqs(node);

    // 'RegionRef' requires nothing an ordinary requirement can state: each region
    // method is optional, with a fixed shape where declared
    if (regionIsRegionRef((INode*)node))
        regionRefCheck(node);

    structCheckCopy(node);
}

// Work both queues until they are empty: first every variant still waiting to be
// laid out, so that no member is checked against an enum that does not yet know
// its size or whether it moves; then the members. Checking a member may begin new
// layouts, and when those finish they work the queues from there, nested, so by
// the time a type's check returns to the use that demanded it, its members are
// checked -- unless it was demanded from inside a layout, or its members were
// already waiting behind the ones being checked.
static void structWorkQueues(void) {
    for (;;) {
        if (structVariantsWaiting && structVariantsNext < structVariantsWaiting->used) {
            StructNode *node = (StructNode*)nodesGet(structVariantsWaiting, structVariantsNext++);
            TypeCheckState tstate;
            tstate.typenode = (INode*)node;
            tstate.fn = NULL;
            tstate.scope = 0;
            // Counted as in flight, so that no member is checked until every one
            // of them is laid out
            ++structLayoutDepth;
            structLayoutVariants(&tstate, node);
            --structLayoutDepth;
            continue;
        }
        if (structMembersWaiting && structMembersNext < structMembersWaiting->used) {
            StructNode *node = (StructNode*)nodesGet(structMembersWaiting, structMembersNext++);
            structCheckMembers(node);
            continue;
        }
        break;
    }
    if (structVariantsWaiting)
        structVariantsWaiting->used = 0;
    structVariantsNext = 0;
    if (structMembersWaiting)
        structMembersWaiting->used = 0;
    structMembersNext = 0;
}

// A layout begins. See "Layout before members" above.
void structLayoutEnter(void) {
    ++structLayoutDepth;
}

// A layout ends. When it was the last one in flight, every waiting variant is laid
// out and every waiting member checked.
void structLayoutExit(void) {
    if (--structLayoutDepth == 0)
        structWorkQueues();
}

// Type check a struct type: its layout. Its members are checked afterwards, once
// no layout is in flight (structCheckMembers).
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

    // An 'extends' base name resolution could not take -- this type is an
    // instance of a generic, or the base is, so one of them was not a declaration
    // until now -- is taken here, before the layout below is settled. Nothing is
    // hooked: no body is resolved after this, so what it brings in is reached as
    // 'self.name' inside this type's own methods, exactly as a member inherited
    // from an instance of a generic trait is.
    StructNode *extbase = NULL;
    if (!(node->flags & EnumType) && node->extendsbase && node->extendsdcl == NULL) {
        if (itypeTypeCheck(pstate, &node->extendsbase) == 0) {
            pstate->typenode = svtypenode;
            return;
        }
        INode *basedcl = itypeGetTypeDcl(node->extendsbase);
        if (structExtendsEligible(node, basedcl, node->extendsbase)) {
            extbase = (StructNode*)basedcl;
            inodeTypeCheckAny(pstate, &basedcl);
        }
    }

    // An enum this one extends is type checked first: the discriminant's width is
    // settled there, and this enum shares the node it is settled on. Name
    // resolution copied the base's variants and took its members already, so there
    // is nothing left to take here; the copies are laid out with this enum's own
    // variants, below. A generic base was written with its arguments, and
    // that instance exists only once the instantiation is type checked, here.
    if ((node->flags & EnumType) && node->extendsbase && node->extendsbase->tag == FnCallTag
        && itypeTypeCheck(pstate, &node->extendsbase) == 0) {
        pstate->typenode = svtypenode;
        return;
    }
    StructNode *enumbase = structEnumBaseDcl(node);
    if (enumbase) {
        INode *basedcl = (INode*)enumbase;
        inodeTypeCheckAny(pstate, &basedcl);
    }

    // Handle when a base trait is specified
    if (node->basetrait) {
        // Name resolution took in a base trait that was a declaration then, and
        // recorded it. One that was not -- an instance of a generic trait, which
        // exists only once the instantiation is type checked here -- is taken in
        // below, through a placeholder field inserted at position 0 as name
        // resolution would have.
        //
        // One more is taken in already: a generic enum's copy of a variant of an
        // enum that is not generic. That variant had its enum's fields spliced in
        // at name resolution, before it was copied, and the copy records the
        // extension's template in their place (structEnumCopyVariant). So an
        // instance of the copy has its fields, and what it records is made the
        // instance of the extension it now belongs to.
        int pending = 1;
        INode **templatep = NULL;
        if (node->traits) {
            INode **traitp;
            uint32_t traitcnt;
            for (nodesFor(node->traits, traitcnt, traitp)) {
                if (isTypeNode(node->basetrait) && *traitp == itypeGetTypeDcl(node->basetrait))
                    pending = 0;
                else if (node->basetrait->tag == FnCallTag && *traitp == (INode*)structBaseTraitDcl(node)) {
                    pending = 0;
                    templatep = traitp;
                }
            }
        }
        if (itypeTypeCheck(pstate, &node->basetrait) == 0) {
            pstate->typenode = svtypenode;
            return;
        }
        StructNode *basetrait = (StructNode*)itypeGetTypeDcl(node->basetrait);
        if (templatep)
            *templatep = (INode*)basetrait;
        if (basetrait->tag != StructTag || !(basetrait->flags & TraitType)) {
            errorMsgNode(node->basetrait, ErrorInvType, "An 'is' names an abstraction, and this is not one");
        }
        else if ((node->flags & HasTagField) != (basetrait->flags & HasTagField)) {
            // An enum's variants are all declared inside it, so nothing outside
            // can join the set later. An enum that adds variants to another's set
            // does not come through here at all: it writes 'extends', which is
            // read into 'extendsbase'.
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

    // Every trait name resolution took in is type checked before this type's
    // layout is settled, as one taken in below is by its placeholder
    if (node->traits) {
        INode **traitp;
        uint32_t traitcnt;
        for (nodesFor(node->traits, traitcnt, traitp))
            inodeTypeCheckAny(pstate, traitp);
    }

    // Iterate backwards through all fields to type check them and to take in any
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
        // An instance of a generic enum is a closed type only now that it exists;
        // a declaration was refused at name resolution
        if (structRefuseClosedIs(node, field, trait, node->basetrait ? structBaseTraitDcl(node) : NULL)) {
            nodelistMakeSpace(&node->fields, fldpos, -1);
            continue;
        }
        CloneState cstate;
        clonePushState(&cstate, (INode*)node, (INode*)node, 0, NULL, NULL);
        structInheritTrait(node, fldpos, trait, &cstate);
        clonePopState();
    }

    // Every placeholder is gone, so the concrete base's members are taken here as
    // name resolution would have taken them, and before the fold clauses below
    if (extbase)
        structEnrichFromBase(node, extbase, 0);

    // A sibling 'use' name resolution could not expand -- this type or the
    // sibling was an instance of a generic -- is expanded here, in the same place
    // in the order. Nothing is hooked, so such a name is reached as 'self.name'
    // inside this type's own methods (see Hazards).
    if (node->siblings) {
        for (nodesFor(node->siblings, cnt, nodesp))
            if (itypeTypeCheck(pstate, &((FieldDclNode*)*nodesp)->vtype) == 0)
                ((FieldDclNode*)*nodesp)->fold->expanded = 1;
        structUseSiblings(node, 0);
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
        // Notice if a field's threadbound or movetype infects the struct. Whether
        // the field moves is asked of itypeIsMove rather than read off its flags,
        // since a tuple carries no flag of its own and moves when one of its
        // elements does.
        ITypeNode *fldtype = (ITypeNode*)itypeGetTypeDcl(((IExpNode*)(*nodesp))->vtype);
        infectFlag |= fldtype->flags & ThreadBound;
        if (itypeIsMove((INode*)fldtype))
            infectFlag |= MoveType;
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
    if (node != structTagWidthDeferred)
        structSetTagWidth(node);

    // The layout is settled, which is what an 'is' asserts about: the fields the
    // abstractions require are declared here, in order, at position 0
    structCheckIsaFields(node);

    // Use inference rules to decide if struct is ThreadBound or a MoveType
    // based on whether its fields are, and whether it supports the .final method.
    //
    // A 'clone' method does not make a move type copyable. The manual's clone
    // makes every copy, but no copy calls it: a copy is bitwise, so a copyable
    // type holding a finalizer or an owner would be finalized once per copy.
    if (namespaceFind(&node->namespace, finalName))
        infectFlag |= MoveType;           // Let's not make copies of finalized objects

    // Populate infection flags in this struct/trait, and recursively to all
    // inherited traits -- but never into a built-in trait, which describes its
    // implementers rather than standing for them: 'Copy' does not move because
    // a type that declared it does
    if (infectFlag) {
        node->flags |= infectFlag;
        StructNode *trait = structBaseTraitDcl(node);
        while (trait && !corelibIsBuiltinTrait((INode*)trait)) {
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

    // Mark the type laid out here: fields are indexed, its own size is known,
    // and the member set is complete -- placeholders were expanded and trait methods
    // inherited during the field walk above. Its members are not checked here
    // at all; they wait until no layout is in flight (structCheckMembers).
    //
    // A type that may be enriched sets its lifecycle aside first, unlowered, for an
    // enrichment taken after this: an enrichment reads TypeChecked to know that
    // the methods themselves may no longer be fit to copy.
    if (!(node->flags & (TraitType | HasTagField)))
        structKeepLifecycle(node);
    node->flags |= TypeChecked;

    // Settle the drop fn as part of the layout, because each method's flow pass
    // asks for it: a by-value 'self', or a local of this type, is finalized at
    // the method's scope exit only if the type has one by then. The fields are
    // laid out, so each field's drop fn is known.
    structSetDropFn(node);

    // The members wait for every layout in flight to finish. An enum's wait
    // ahead of those of the variants it lays out below, in the order written.
    structWait(&structMembersWaiting, node);

    // An enum's size, and whether its values move or stay on their thread, are
    // its variants', so its layout is not finished until theirs are. Each one
    // not yet begun is laid out now -- unless one is in flight already, which
    // means this enum was demanded from inside that variant's layout. A sibling
    // laid out here could then hold that variant by value and find it unfinished,
    // which would be a cycle only by the order the walk happened to take, so the
    // rest wait until the layout in flight is done (structLayoutExit).
    if (structIsClosedSet(node)) {
        if (structVariantInFlight(node))
            structWait(&structVariantsWaiting, node);
        else
            structLayoutVariants(pstate, node);
    }

    pstate->typenode = svtypenode;
}

// Map a struct's members onto a base struct's vtable slots, registering nothing
// Return the implementation if it successfully type matches, NULL if not
static VtableImpl *structMapVtableImpl(StructNode *basenode, StructNode *strnode) {
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

    return impl;
}

// Add a vtable implementation to a base struct's vtable
// Return 1 if it successfully type matches, 0 if not
int structAddVtableImpl(StructNode *basenode, StructNode *strnode) {
    VtableImpl *impl = structMapVtableImpl(basenode, strnode);
    if (impl == NULL)
        return 0;
    nodesAdd(&basenode->vtable->impl, (INode*)impl);
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

    // Two enums never substitute for each other -- see structMatches -- and a
    // virtual reference is a view of a value, so it is a substitution too. Refused
    // before the mapping below, which would find every slot: an extension's members
    // are its base's.
    if ((trait->flags & EnumType) && (strnode->flags & EnumType))
        return NoMatch;

    if (trait->vtable == NULL)
        structMakeVtable(trait);

    // A trait cannot fill another trait's slots with its own methods: none of
    // them is generated, since a default is cloned into each implementer and an
    // abstract method has no body. What a plain reference to an enum points at is
    // one of its variants, so every variant is mapped instead, and the coercion
    // picks the variant's vtable by the tag (genlConvert), as it does for a
    // virtual reference to the enum itself. The enum still has to comply in its
    // own right, as any type does; its mapping is only not kept. An open trait
    // has no tag to pick with, so a plain reference to one converts to no other
    // trait.
    if (strnode->flags & TraitType) {
        if (!(strnode->flags & HasTagField) || strnode->derived == NULL || strnode->derived->used == 0
            || structMapVtableImpl(trait, strnode) == NULL)
            return NoMatch;
        INode **varp;
        uint32_t varcnt;
        for (nodesFor(strnode->derived, varcnt, varp)) {
            if (structVirtRefMatches(trait, (StructNode*)*varp) == NoMatch)
                return NoMatch;
        }
        return ConvSubtype;
    }

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

    // An enrichment and its base substitute for each other in BOTH directions,
    // under every constraint, at no cost: 'extends' may not touch the fields, so
    // the two have one representation and a recast is the whole of the
    // conversion. This is not subtyping in either direction -- neither type is an
    // abstraction of the other -- which is why it is answered here, ahead of the
    // trait test, rather than by finding a supertype.
    if (structExtendsEquiv((INode*)to, fromdcl))
        return CastSubtype;

    // TWO ENUMS NEVER SUBSTITUTE FOR EACH OTHER, in either direction, and an enum
    // and the enum it extends least of all.
    //
    // Adding variants to a set makes a SUPERTYPE -- every value of the base is a
    // value of the extension, and no value of the extension is a value of the base
    // -- which is the reverse of the direction every subtype relationship in Cone
    // runs. Cone does not take that direction here: the two are distinct types with
    // distinct variant sets, so a match on either stays exhaustive over its own.
    //
    // Refused here rather than left to the structural test below, which would say
    // yes: an extension's fields are clones of its base's and its members are the
    // base's, so the two are structurally identical in both directions. See
    // compiler/c/doc/nodes/struct.md, "An enum extending an enum".
    if (fromdcl->tag == StructTag && (to->flags & EnumType) && (fromdcl->flags & EnumType))
        return NoMatch;

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

    // An enum's values are its own variants and nothing else. A variant's enum is
    // the one its 'basetrait' names -- for an extension's copy of a base variant,
    // the extension -- so a variant the walk above did not reach is not a value of
    // this enum, whatever its shape. Refused here rather than left to the
    // structural test below, which would say yes to a variant of a base enum or of
    // an extension of this one: the discriminant is one shared node and the common
    // fields are clones, so the prefix matches. An '@unsized' enum is not walked
    // above, and it asks the same question of its variants by reference.
    if (to->flags & EnumType) {
        StructNode *super = (StructNode *)fromdcl;
        while (super->basetrait) {
            StructNode *base = (StructNode*)itypeGetTypeDcl(super->basetrait);
            if (to == base)
                return constraint == Coercion && !(to->flags & SameSize) ? NoMatch : CastSubtype;
            super = base;
        }
        return NoMatch;
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

    // An enrichment and its base are not one another's supertype, but either
    // stands for both: they have one representation and substitute freely, so an
    // inferred type in common is whichever was seen first
    if (structExtendsEquiv(type1, type2))
        return type1;

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

    // As in structFindSuper: either name stands for both
    if (structExtendsEquiv(type1, type2))
        return type1;

    // A reference's value type need not be a struct (&i32, &f64), and no other
    // value type has a supertype through a reference. structFindSuper is only
    // reached with two structs; this is reached with any two pointees.
    if (typ1->tag != StructTag || typ2->tag != StructTag)
        return NULL;

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
