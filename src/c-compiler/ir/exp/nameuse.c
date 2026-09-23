/** Name and Member Use nodes.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new name use node
NameUseNode *newNameUseNode(Name *namesym) {
    NameUseNode *name;
    newNode(name, NameUseNode, NameUseTag);
    // Type checking replaces this with the declaration's type. A use inside a
    // template that is only ever cloned -- a trait's default method, a generic's
    // body -- is never type checked at all, and keeps it.
    name->vtype = unknownType;
    name->dclnode = NULL;
    name->namesym = namesym;
    return name;
}

// The same, positioned on an existing node rather than on wherever the lexer
// happens to be. A name use the parser synthesizes after it has consumed the
// construct it belongs to would otherwise carry the position of whatever the
// lexer had moved on to.
NameUseNode *newNameUseFromLex(Name *namesym, INode *lexnode) {
    NameUseNode *name = newNameUseNode(namesym);
    inodeLexCopy((INode*)name, lexnode);
    return name;
}

// Create a working variable for a value we intend to reuse later
// The vardcl is appended to a list of nodes, and the nameuse node to it is returned
INode *newNameUseAndDcl(Nodes **nodesp, INode *val, uint16_t scope) {
    VarDclNode *var = (VarDclNode*)newVarDclFull(tempName, VarDclTag, unknownType, (INode*)immPerm, val);
    var->scope = scope;
    nodesAdd(nodesp, (INode*)var);
    NameUseNode *varuse = newNameUseNode(tempName);
    varuse->dclnode = (INode*)var;
    return (INode*)varuse;
}

// Create a new nameuse node pointing to an existing dclnode
INode *newNameUseFromDclNode(INode *dclnode, INode *lexnode) {
    Name *name = inodeGetName(dclnode);
    NameUseNode *fnuse = newNameUseNode(name);
    inodeLexCopy((INode*)fnuse, lexnode);
    fnuse->dclnode = dclnode;
    if (!isTypeNode(dclnode))
        fnuse->vtype = ((IExpNode*)dclnode)->vtype;
    return (INode *)fnuse;
}

// Create a member name: a field, method or operator name to be applied to a
// value. It is an ordinary name use that name resolution never sees -- it
// lives in a call's member slot, which fnCallNameRes leaves alone -- and it
// stays bound to nothing until fnCallLowerMethod selects the member against
// the receiver's type.
NameUseNode *newMemberUseNode(Name *namesym) {
    return newNameUseNode(namesym);
}

// Clone NameUse
INode *cloneNameUseNode(CloneState *cstate, NameUseNode *node) {
    NameUseNode *newnode;
    newnode = memAllocBlk(sizeof(NameUseNode));
    memcpy(newnode, node, sizeof(NameUseNode));
    newnode->dclnode = cloneDclFix(node->dclnode);
    return (INode *)newnode;
}

// The declaration a name use names, at the end of its chain of names,
// or NULL while it is unresolved
INode *nameUseGetDcl(NameUseNode *name) {
    INode *dcl = name->dclnode;
    // An alias stands for what its target names, so a use bound to one answers
    // for that: a folded method's alias is asked and answers as the method
    while (dcl) {
        if (isNameUseNode(dcl))
            dcl = ((NameUseNode*)dcl)->dclnode;
        else if (dcl->tag == AliasDclTag)
            dcl = ((AliasDclNode*)dcl)->target;
        else
            break;
    }
    return dcl;
}

// The group a name use belongs to, asked of the declaration it names: a
// variable, function, overload set, field or constant makes it an expression; a
// macro or a generic parameter makes it a meta node; every other declaration
// makes it a type. That last is a fallthrough rather than a claim: a module is
// not a type, and a use of its name answers as one. A name bound to nothing --
// not yet resolved, or a member name before type check selects the member
// against the receiver's type -- is in no group: not an expression, not a
// type, not a meta node.
NodeGroup nameUseGroup(NameUseNode *name) {
    INode *dcl = nameUseGetDcl(name);
    if (dcl == NULL)
        return StmtGroup;
    switch (dcl->tag) {
    case VarDclTag:
    case FnDclTag:
    case FnOverloadDclTag:
    case FieldDclTag:
    case ConstDclTag:
        return ExpGroup;
    case MacroDclTag:
    case GenVarDclTag:
        return MetaGroup;
    default:
        return TypeGroup;
    }
}

// Does this node name a declaration with the given tag? No for a node that is
// not a name use, and for a name use bound to nothing yet
int nameUseNames(INode *node, uint16_t dcltag) {
    if (!isNameUseNode(node))
        return 0;
    INode *dcl = nameUseGetDcl((NameUseNode*)node);
    return dcl != NULL && dcl->tag == dcltag;
}

// Serialize a name use node
void nameUsePrint(NameUseNode *name) {
    inodeFprint("%s", &name->namesym->namestr);
}

// Handle name resolution for name use references: point dclnode at the name's
// declaration. That is all a use needs -- whether it is a type, a value or a
// macro is asked of the declaration (nameUseGroup), and a bare field name is
// lowered to 'self.field' by type check, which has the type that lowering needs.
//
// Every name that reaches here is a bare one. A name reached through a
// namespace was written as a member access and is bound by fnCallNameRes, which
// collapses the path; the node it leaves behind arrives here already resolved.
void nameUseNameRes(NameResState *pstate, NameUseNode **namep) {
    NameUseNode *name = *namep;

    // If name is already "resolved", we are done. This happens for de-sugaring
    // logic that creates pre-resolved phantom variables, and for a name the
    // path collapse bound.
    if (name->dclnode)
        return;

    // A bare name is already hooked into the global name table by whichever
    // scope owns it, innermost last, so this is one pointer read and no walk
    name->dclnode = name->namesym->node;

    if (!name->dclnode) {
        // A pattern's bare root may be a variant of the matched value's enum,
        // which only type check knows: castPatternBind binds it or reports it
        if (name->flags & FlagPattern)
            return;
        // A bare name a clause of this module would have folded in may be a
        // re-export lost round a cycle of imports, and the report says so where it is
        modNameMissing(pstate->mod, pstate->mod, name->namesym, (INode*)name, ErrorUnkName,
            "The name %s does not refer to a declared name", &name->namesym->namestr);
        return;
    }

    // In a method, a bare member name means 'self.member', and type check reaches
    // it through the method's own receiver. A macro method's body is expanded into
    // whichever function uses it, whose self -- if it has one -- is not this
    // type's, so the body has to write 'self.member' itself. Refused here, where
    // the name is known to be a member, rather than left to expand into a
    // reference to the wrong receiver.
    if (pstate->macromethod && inodeIsMember(name->dclnode))
        errorMsgNode((INode*)name, ErrorBareMbr,
            "In a macro method, %s must be reached through self, as self.%s",
            &name->namesym->namestr, &name->namesym->namestr);
}

// Report a use of a member of a generic type itself, such as 'Box.stat' on a
// 'struct Box[T]', and return 1 if it is one. The generic has no members of its
// own to reach: every instance has its copy, the only one given a symbol, so the
// use must say which instance. A use inside the generic's body reaches here
// already re-pointed at its instance's member by the clone (cloneStructNode).
int nameUseTemplateMember(NameUseNode *name, INode *dcl) {
    INode *owner = inodeGetOwner(dcl);
    if (owner == NULL || owner->tag != StructTag || ((StructNode*)owner)->genericinfo == NULL)
        return 0;
    Name *ownername = ((StructNode*)owner)->namesym;
    errorMsgNode((INode*)name, ErrorArgCount,
        "%s is generic, so %s belongs to each of its instances, named with type arguments as %s[...]; reaching a member through an instance is not built.",
        &ownername->namestr, &name->namesym->namestr, &ownername->namestr);
    return 1;
}

// Handle type check for variable/function name use references
void nameUseTypeCheck(TypeCheckState *pstate, NameUseNode **namep) {
    NameUseNode *name = *namep;
    // A use bound to an alias is a use of what the alias stands for, from here
    // on: everything below reads the declaration's type and tag
    AliasDclNode *alias = name->dclnode->tag == AliasDclTag ? (AliasDclNode*)name->dclnode : NULL;
    if (alias) {
        INode *dcl = aliasDclResolve(name->dclnode);
        if (dcl == NULL) {
            // The fold that made the alias failed to bind it, and said so
            name->vtype = errorType;
            return;
        }
        name->dclnode = dcl;
    }

    // A name a global's 'use' clause folded into this module is reached through
    // that global, so the use is lowered to 'global.name' -- the path the author
    // could have written, which is why nothing after this is new. Both a field
    // and a method arrive here; a method being CALLED is lowered by
    // fnCallTypeCheck instead, before it reads the callee, so what is left here
    // is a member read. Qualification makes no difference: 'mymod.speed' names
    // the module's binding, and the binding is still reached through the global.
    if (alias && alias->through) {
        *((FnCallNode**)namep) = aliasDclThroughAccess(alias, (INode*)name);
        inodeTypeCheckAny(pstate, (INode**)namep);
        return;
    }
    if (nameUseTemplateMember(name, name->dclnode)) {
        name->vtype = errorType;
        return;
    }
    // An overload name has no value of its own: it names a set of concrete
    // declarations. Only a call may use it, and the call type check selects and
    // rewrites this use to the concrete declaration before reaching here.
    if (name->dclnode->tag == FnOverloadDclTag) {
        errorMsgNode((INode*)name, ErrorOverloadUse,
            "The overload name %s may only be used as the name being called. Use a concrete name for its value.",
            &name->namesym->namestr);
        name->vtype = unknownType;
        return;
    }
    // A bare field name inside a method means 'self.field'. This is lowering --
    // it builds a call node and takes its type from what that call resolves to
    // -- so it belongs to type check. Name resolution did it, with no type to
    // work from. Its counterpart in fnCallTypeCheck covers the disjoint case, a
    // bare *method* name being called.
    //
    // 'bare' is the whole of the condition: a field named through its type,
    // 'Gadget.w', asked for that type's field and not for this method's self.
    if (name->dclnode->tag == FieldDclTag && (name->dclnode->flags & FlagMethFld)
        && !(name->flags & FlagQualified)) {
        // Only a method has a receiver to reach a field through. A field's own
        // default value is analyzed with no function around it, so a name that
        // resolved to a sibling field there has nothing to qualify it.
        if (pstate->fn == NULL || !(pstate->fn->flags & FlagMethFld)) {
            errorMsgNode((INode*)name, ErrorUnkName,
                "%s is a field, and there is no self here to reach it through.",
                &name->namesym->namestr);
            name->vtype = errorType;
            return;
        }
        // Build a resolved 'self' node and re-read the name as a member of it
        NameUseNode *selfnode = newNameUseNode(selfName);
        copyNodeLex(selfnode, name);
        selfnode->dclnode = nodesGet(((FnSigNode*)pstate->fn->vtype)->parms, 0);
        selfnode->vtype = ((VarDclNode*)selfnode->dclnode)->vtype;
        FnCallNode *fncall = newFnCallNode((INode *)selfnode, 0);
        fncall->methfld = (INode*)name;
        copyNodeLex(fncall, name); // Copy lexer info into injected node in case it has errors
        *((FnCallNode**)namep) = fncall;
        inodeTypeCheckAny(pstate, (INode**)namep);
        return;
    }

    // Rule 1: reaching a name analyzes the declaration it names, so what is read
    // below is a finished type rather than whatever source order happened to
    // leave behind. A declaration already type checked returns at once; one still
    // under type check returns having established its own type, which is all a use
    // needs and is what lets two functions call each other.
    inodeTypeCheckAny(pstate, &name->dclnode);

    // Rule 6: a constant, and a variable or field whose type is inferred, take
    // their type *from* the value that may name them back, so re-entering one
    // before that type exists leaves nothing to answer with. Every other
    // declaration has established its type by now, so an unknown type here is
    // exactly a definition depending on itself.
    INode *dcl = name->dclnode;
    if (((IExpNode*)dcl)->vtype == unknownType
        && (dcl->flags & TypeChecking) && !(dcl->flags & TypeChecked)) {
        errorMsgNode((INode*)name, ErrorCircular,
            "%s is defined in terms of itself, so it has no type to give.",
            &name->namesym->namestr);
        name->vtype = errorType;
        return;
    }

    name->vtype = ((IExpNode*)dcl)->vtype;
}

// Handle type check for type name use references
void nameUseTypeCheckType(TypeCheckState *pstate, NameUseNode **namep) {
    // Do type check on the type declaration this refers to,
    // to ensure it is correct and knows about its infectious constraints
    // Guards are in place to ensure this only will be done once, as early as possible.
    //
    // Naming a type that is still being laid out is not itself an error: the
    // name resolves to the same declaration either way. What such a type cannot
    // answer is its size, and that is asked by whatever wants to hold a value of
    // it -- a field, a variable, an array element -- not here.
    NameUseNode *name = *namep;
    inodeTypeCheckAny(pstate, &name->dclnode);
    // A type a fold brought in -- a variant a module's 'use' of its enum folded,
    // or a type an import folded -- is named through an alias, which has nothing
    // of its own to check. The type it stands for is what has to be analyzed, as
    // it is when the type is named directly. A typedef's alias checks its own
    // target, which is a type expression and reaches here by itself.
    if (name->dclnode->tag == AliasDclTag && !(name->dclnode->flags & FlagTypeAlias)) {
        INode *dcl = aliasDclResolve(name->dclnode);
        if (dcl)
            inodeTypeCheckAny(pstate, &dcl);
    }
}

// Ensure variable has a usable value
void nameuseFlow(FlowState *fstate, NameUseNode **nodep) {
    NameUseNode *node = *nodep;
    VarDclNode *vardclnode = (VarDclNode *)((NameUseNode*)node)->dclnode;
    if (vardclnode->tag != VarDclTag)
        return;
    if (!(vardclnode->flowtempflags & VarInitialized))
        errorMsgNode((INode*)node, ErrorMove, "This variable has not been initialized. There is no value to use.");
    else if (vardclnode->flowtempflags & VarMoved)
        errorMsgNode((INode*)node, ErrorMove, "This variable's value has been moved out. It is no longer there to use.");
}