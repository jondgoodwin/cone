/** Module traits, and a module's conformance to one
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

// Create a new module trait node
ModTraitNode *newModTraitNode(Name *namesym) {
    ModTraitNode *trait;
    newNode(trait, ModTraitNode, ModTraitTag);
    trait->namesym = namesym;
    dclInfoInit(&trait->dclinfo);
    trait->nodes = newNodes(8);
    namespaceInit(&trait->namespace, 8);
    return trait;
}

// Add a parsed member to the trait. A member is a requirement or a default under
// one name, so a name written twice is refused as it is in a module
void modTraitAddMember(ModTraitNode *trait, INode *member) {
    Name *name = inodeGetName(member);
    if (name == NULL || name == anonName)
        return;
    if (namespaceAdd(&trait->namespace, name, member)) {
        errorMsgNode(member, ErrorDupName, "%s is already a member of module trait %s.",
            &name->namestr, &trait->namesym->namestr);
        return;
    }
    nodesAdd(&trait->nodes, member);
    dclInfoJoin(member, (INode*)trait);
}

// Serialize a module trait: its name, then each member
void modTraitPrint(ModTraitNode *trait) {
    inodeFprint("mod trait %s", &trait->namesym->namestr);
    dclInfoPrint((INode*)trait);
    inodeFprint(" {");
    inodePrintNL();
    inodePrintIncr();
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(trait->nodes, cnt, nodesp)) {
        inodePrintIndent();
        inodePrintNode(*nodesp);
        inodePrintNL();
    }
    inodePrintDecr();
    inodePrintIndent();
    inodeFprint("}");
}

// Is this member a default, which a conforming module that does not declare the
// name takes a copy of? A function with a body, or a global with an initialiser
static int modTraitIsDefault(INode *member) {
    if (member->tag == FnDclTag)
        return ((FnDclNode*)member)->value != NULL;
    return ((VarDclNode*)member)->value != NULL;
}

// Resolve the trait's members. The declaring module's names are hooked already,
// whether its own walk reached the trait or a conforming module demanded it
// (modTraitNameResDemand); the trait's own are hooked over them, so a default's
// body names another member bare. Such a use binds to the member, and a copy of
// the body is re-pointed at what the conforming module has under that name
// (modTraitConform).
void modTraitNameRes(NameResState *pstate, ModTraitNode *trait) {
    if (trait->flags & (NameResolved | NameResolving))
        return;
    trait->flags |= NameResolving;
    // A default's body is expanded into every conforming module, so what it
    // names from the declaring module is marked for a library to export
    // (fnDclIsExpanded)
    INode *svtypenode = pstate->typenode;
    pstate->typenode = (INode*)trait;
    nametblHookPush();
    nametblHookNamespace(&trait->namespace);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(trait->nodes, cnt, nodesp))
        inodeNameRes(pstate, nodesp);
    nametblHookPop();
    pstate->typenode = svtypenode;
    trait->flags = (trait->flags & ~NameResolving) | NameResolved;
}

// Resolve a trait a module conforms to, in the scope of the module that declares
// it, which may not be the one asking. Every module's folds have run, so the
// declaring module's namespace is complete
static void modTraitNameResDemand(NameResState *pstate, ModTraitNode *trait) {
    if (trait->flags & (NameResolved | NameResolving))
        return;
    ModuleNode *mod = dclInfoGetModule((INode*)trait);
    ModuleNode *svmod = pstate->mod;
    pstate->mod = mod;
    modHook(NULL, mod);
    modTraitNameRes(pstate, trait);
    modHook(mod, NULL);
    pstate->mod = svmod;
}

// Type check the trait: each member's signature, and each global. A default's
// body is left alone, as a struct trait's default method's is (fnDclTypeCheck):
// it is checked in every copy, where its names are the conforming module's
void modTraitTypeCheck(TypeCheckState *pstate, ModTraitNode *trait) {
    TypeCheckState tstate;
    tstate.typenode = (INode*)trait;
    tstate.fn = NULL;
    tstate.scope = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(trait->nodes, cnt, nodesp))
        inodeTypeCheckAny(&tstate, nodesp);
}

// What a binding of the module's namespace stands for, looked through a chain
// of fold aliases
static INode *modTraitBindingDcl(INode *binding) {
    return aliasDclResolve(binding);
}

// Does the declaration a module has under a member's name have the member's
// kind -- a function or an overload set for a function, a global for a global?
// Only then is a default's body re-pointed at it, and its shape compared
static int modTraitSameKind(INode *member, INode *dcl) {
    if (dcl == NULL)
        return 0;
    if (member->tag == FnDclTag)
        return dcl->tag == FnDclTag || dcl->tag == FnOverloadDclTag;
    return dcl->tag == VarDclTag;
}

// Resolve what the module's 'is' names. It is a name the module can already
// reach -- its own namespace, then the registry its parent is -- looked up and
// never loaded, as 'extends' is (modExtendsResolve). Where 'report' is 0, what
// cannot be resolved is left for the pass that reports, and nothing is said
static ModTraitNode *modTraitResolveIs(ModuleNode *mod, int report) {
    NameUseNode *name = (NameUseNode*)mod->traitname;
    ModuleNode *parent = (ModuleNode*)mod->dclinfo.owner;
    INode *binding = namespaceFind(&mod->namespace, name->namesym);
    if (binding == NULL && parent != NULL)
        binding = namespaceFind(&parent->namespace, name->namesym);
    INode *found = binding ? aliasDclResolve(binding) : NULL;
    if (found != NULL && found->tag == ModTraitTag) {
        name->dclnode = found;
        return (ModTraitNode*)found;
    }
    if (!report)
        return NULL;
    if (binding == NULL) {
        errorMsgNode((INode*)name, ErrorUnkName,
            "%s names nothing this module can reach. A module's 'is' names a module trait of its own or of its parent, or one an import folds in: 'import hosts use %s'.",
            &name->namesym->namestr, &name->namesym->namestr);
        return NULL;
    }
    if (found != NULL && found->tag == StructTag && (found->flags & TraitType))
        errorMsgNode((INode*)name, ErrorModIs,
            "%s is a trait of a struct. A module conforms to a module trait, declared 'mod trait %s'.",
            &name->namesym->namestr, &name->namesym->namestr);
    else if (found != NULL && found->tag == ModuleTag)
        errorMsgNode((INode*)name, ErrorModIs,
            "%s is a module. A module conforms to a module trait with 'is', and reuses a module with 'extends'.",
            &name->namesym->namestr);
    else
        errorMsgNode((INode*)name, ErrorModIs,
            "%s is not a module trait. A module's 'is' names the module trait it conforms to.",
            &name->namesym->namestr);
    return NULL;
}

// Resolve the trait a module conforms to, and clone in each default it does not
// declare (modtrait.h).
//
// A member the module already has a name for is met by that name: a declaration
// of the module, or a name its folds brought in -- what it extends among them --
// since conformance is a question about names and signatures, as it is for a
// struct trait. Whether that declaration has the member's shape is asked in
// type check (modTraitCheck), once both have types. A member the module has no
// name for is taken from the trait where it is a default, and is a missing
// requirement where it is not.
//
// A default is copied the way a generic type's members are (cloneStructNode): a
// shell for each, bound in the module, before any body is copied, with every
// member mapped to what the module has under its name. A body naming another
// member then names the module's -- its own declaration, or the copy of another
// default -- and a body naming anything else keeps the binding it had in the
// trait's scope. The copy is owned by the module, so it is spelled after it,
// private unless the trait wrote 'pub', and generated wherever the module is.
// The copies arrive resolved and are appended to the module's nodes, which is
// why modNameRes leaves the last 'ntaken' of them alone.
//
// WHEN it runs is what makes a copy a declaration of the module to everyone who
// reads the module's namespace. It runs at the end of the module's own fold pass
// (modFoldNames), where the module's namespace is complete and before any module
// folding FROM it reads it -- the fold pass is dependency-first -- so a module
// extending this one takes the copies as the base's declarations, sharing a
// default global's one storage, and an importer's 'use *' folds them like any
// other public name. That holds because imports form a DAG [Jon 23 Sep]. The
// trait's own module must have finished its folds too, since the trait's bodies
// are resolved in its scope; where it has not (a parent's trait, the parent still
// folding), or the 'is' does not resolve yet, the attempt is left, and the pass
// after the folds makes it and reports what is wrong ('report' set).
void modTraitConform(NameResState *pstate, ModuleNode *mod, int report) {
    if (mod->traitname == NULL || mod->trait != NULL)
        return;
    ModTraitNode *trait = modTraitResolveIs(mod, report);
    if (trait == NULL)
        return;
    ModuleNode *traitmod = dclInfoGetModule((INode*)trait);
    if (!report && traitmod != mod && traitmod->folding)
        return;
    mod->trait = trait;
    modTraitNameResDemand(pstate, trait);

    INode **nodesp;
    uint32_t cnt;
    CloneState cstate;
    clonePushState(&cstate, mod->traitname, NULL, 0, NULL, NULL);
    uint32_t dclpos = cloneDclPush();
    uint32_t firstcopy = mod->nodes->used;
    for (nodesFor(trait->nodes, cnt, nodesp)) {
        INode *member = *nodesp;
        Name *name = inodeGetName(member);
        INode *binding = namespaceFind(&mod->namespace, name);
        if (binding != NULL) {
            if (modTraitSameKind(member, modTraitBindingDcl(binding)))
                cloneDclSetMap(member, binding);
            continue;
        }
        if (!modTraitIsDefault(member)) {
            errorMsgNode(mod->traitname, ErrorModTraitMissing,
                "Module %s does not declare %s %s, which module trait %s requires and gives no default for.",
                &mod->namesym->namestr, member->tag == FnDclTag ? "function" : "global",
                &name->namestr, &trait->namesym->namestr);
            continue;
        }
        INode *copy = member->tag == FnDclTag
            ? (INode*)cloneFnDclShell((FnDclNode*)member)
            : (INode*)cloneVarDclShell((VarDclNode*)member);
        copy->instnode = mod->traitname;
        dclInfoJoin(copy, (INode*)mod);
        nodesAdd(&mod->nodes, copy);
        namespaceSet(&mod->namespace, name, copy);
        cloneDclSetMap(member, copy);
    }
    // Every shell is bound and mapped, so now the bodies, in the same order
    uint32_t copyat = firstcopy;
    for (nodesFor(trait->nodes, cnt, nodesp)) {
        INode *member = *nodesp;
        if (copyat >= mod->nodes->used)
            break;
        INode *copy = nodesGet(mod->nodes, copyat);
        if (inodeGetName(copy) != inodeGetName(member))
            continue;
        if (member->tag == FnDclTag)
            cloneFnDclFill(&cstate, (FnDclNode*)copy, (FnDclNode*)member);
        else
            cloneVarDclFill(&cstate, (VarDclNode*)copy, (VarDclNode*)member);
        ++copyat;
    }
    cloneDclPop(dclpos);
    clonePopState();
    mod->ntaken = mod->nodes->used - firstcopy;
}

// The one candidate under a module's binding for a function member whose
// signature is the member's, or NULL. Each candidate is analyzed first, since it
// may not have been reached yet
static FnDclNode *modTraitFindFn(TypeCheckState *pstate, INode *dcl, FnDclNode *member) {
    INode **candidatep;
    uint32_t cnt;
    if (dcl->tag == FnDclTag) {
        candidatep = &dcl;
        cnt = 1;
    }
    else {
        Nodes *overloads = ((FnOverloadDclNode*)dcl)->overloads;
        cnt = overloads->used;
        candidatep = cnt ? &nodesGet(overloads, 0) : NULL;
    }
    FnDclNode *found = NULL;
    TypeCheckState tstate;
    tstate.typenode = NULL;
    tstate.fn = NULL;
    tstate.scope = 0;
    while (cnt--) {
        FnDclNode *candidate = (FnDclNode*)*candidatep++;
        // A generic function has a signature per instance, and none a host could call
        if (candidate->genericinfo)
            continue;
        INode *node = (INode*)candidate;
        inodeTypeCheckAny(&tstate, &node);
        if (fnSigEqual((FnSigNode*)candidate->vtype, (FnSigNode*)member->vtype))
            found = candidate;
    }
    return found;
}

// Check the shape of what the module declares for each member (modtrait.h).
// What differs is reported where conformance is written, at the 'is' on the
// 'mod' line, naming the member and what the trait requires of it. A copy of a
// default has the member's shape by construction and is passed over.
void modTraitCheck(TypeCheckState *pstate, ModuleNode *mod) {
    ModTraitNode *trait = mod->trait;
    if (trait == NULL)
        return;
    INode *traitnode = (INode*)trait;
    inodeTypeCheckAny(pstate, &traitnode);
    int errorsOnEntry = errors;

    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(trait->nodes, cnt, nodesp)) {
        INode *member = *nodesp;
        Name *name = inodeGetName(member);
        INode *binding = namespaceFind(&mod->namespace, name);
        if (binding == NULL)
            continue;  // reported as missing by modTraitConform
        INode *dcl = modTraitBindingDcl(binding);
        if (dcl == NULL || dcl->instnode == mod->traitname)
            continue;
        char *kind = member->tag == FnDclTag ? "function" : "global";
        if (!modTraitSameKind(member, dcl)) {
            errorMsgNode(mod->traitname, ErrorModTraitMismatch,
                "Module trait %s requires %s to be a %s, and what module %s has under that name is not one.",
                &trait->namesym->namestr, &name->namestr, kind, &mod->namesym->namestr);
            continue;
        }
        if (member->tag == FnDclTag) {
            if (modTraitFindFn(pstate, dcl, (FnDclNode*)member) == NULL)
                errorMsgNode(mod->traitname, ErrorModTraitMismatch,
                    "Module %s declares %s, but not with the signature module trait %s requires of it.",
                    &mod->namesym->namestr, &name->namestr, &trait->namesym->namestr);
            continue;
        }
        VarDclNode *global = (VarDclNode*)dcl;
        VarDclNode *required = (VarDclNode*)member;
        INode *node = dcl;
        inodeTypeCheckAny(pstate, &node);
        if (!itypeIsSame(global->vtype, required->vtype))
            errorMsgNode(mod->traitname, ErrorModTraitMismatch,
                "Module %s declares global %s, but not of the type module trait %s requires of it.",
                &mod->namesym->namestr, &name->namestr, &trait->namesym->namestr);
        else if (!permIsSame((INode*)global->perm, (INode*)required->perm))
            errorMsgNode(mod->traitname, ErrorModTraitMismatch,
                "Module %s declares global %s with a permission other than the one module trait %s requires of it.",
                &mod->namesym->namestr, &name->namestr, &trait->namesym->namestr);
    }

    // A copy's body was written against the trait's members, and where what the
    // module has differs from them, checking the copy reports that difference
    // again, inside the trait. The mismatch has been said once, where 'is' is
    // written, so the copies are passed over; nothing is generated after an error
    if (errors != errorsOnEntry) {
        for (uint32_t pos = mod->nodes->used - mod->ntaken; pos < mod->nodes->used; ++pos)
            nodesGet(mod->nodes, pos)->flags |= TypeChecked;
    }
}
