/** Handling for alias declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>

// Create an alias under a local spelling for what 'target' names
AliasDclNode *newAliasDclNode(Name *namesym, INode *target) {
    AliasDclNode *node;
    newNode(node, AliasDclNode, AliasDclTag);
    node->namesym = namesym;
    node->target = target;
    node->through = NULL;
    node->flags |= FlagPub | FlagMethFld;
    return node;
}

// Create an alias for a name of another namespace, which is what an import's
// fold makes. Neither flag applies: it stands for a declaration reached with no
// receiver at all -- a module has one instance and no address to go through --
// and its visibility is its own, so it starts private and only a 're-export'
// says otherwise. That default is the transit rule: a fold is private to the
// module that made it, so what a third module sees is what was re-exported.
AliasDclNode *newNameAliasDclNode(Name *namesym, INode *target) {
    AliasDclNode *node;
    newNode(node, AliasDclNode, AliasDclTag);
    node->namesym = namesym;
    node->target = target;
    node->through = NULL;
    return node;
}

// Create an alias for a type expression, which is what 'typedef' declares.
// Neither flag the folded case sets applies: it stands for a type rather than
// for a member reached through a receiver, and its visibility is its own, so
// it starts private and the 'pub' the parser saw is what sets the bit.
AliasDclNode *newTypeAliasDclNode(Name *namesym, INode *typeexp) {
    AliasDclNode *node;
    newNode(node, AliasDclNode, AliasDclTag);
    node->namesym = namesym;
    node->target = typeexp;
    node->through = NULL;
    node->flags |= FlagTypeAlias;
    return node;
}

// Clone an alias. The original is mapped to the copy, so that a cloned method
// body whose name use was bound to the original is re-pointed at the copy, as
// a use of any other cloned declaration is.
INode *cloneAliasDclNode(CloneState *cstate, AliasDclNode *node) {
    AliasDclNode *newnode = memAllocBlk(sizeof(AliasDclNode));
    memcpy(newnode, node, sizeof(AliasDclNode));
    newnode->target = cloneNode(cstate, node->target);
    cloneDclSetMap((INode*)node, (INode*)newnode);
    return (INode*)newnode;
}

// Serialize an alias
void aliasDclPrint(AliasDclNode *node) {
    inodeFprint("use %s", &node->namesym->namestr);
    if (node->through) {
        inodeFprint(" through ");
        inodePrintNode(node->through);
    }
    if (node->target) {
        inodeFprint(" = ");
        inodePrintNode(node->target);
    }
}

// Name resolution of an alias. A folded name's target is a member name, bound
// where the fold that made the alias is expanded, exactly as a call's member
// slot is; a type alias's target is a type expression and is resolved here.
void aliasDclNameRes(NameResState *pstate, AliasDclNode *node) {
    if (node->flags & FlagTypeAlias)
        inodeNameRes(pstate, &node->target);
}

// Type check an alias. A folded name's target is checked as itself, by whatever
// reaches it through the alias; a type alias's target is a type expression no
// other node owns, so it is checked here.
void aliasDclTypeCheck(TypeCheckState *pstate, AliasDclNode *node) {
    if (node->flags & FlagTypeAlias)
        itypeTypeCheck(pstate, &node->target);
}

// The next alias along a chain, or NULL where the chain ends at something else
static AliasDclNode *aliasDclNext(AliasDclNode *node) {
    INode *target = node->target;
    INode *dcl = (target && isNameUseNode(target)) ? ((NameUseNode*)target)->dclnode : target;
    return (dcl && dcl->tag == AliasDclTag) ? (AliasDclNode*)dcl : NULL;
}

// Report a chain of type aliases that comes back to itself, and break it.
//
// A name defined in terms of itself names no type, and every reader walks the
// chain to its end, so the cycle has to be cut before anything follows it: two
// pointers at two speeds meet only inside a cycle, and the target then becomes
// 'unknown' so the chain ends and the name still answers as a type.
void aliasDclCheckCycle(AliasDclNode *node) {
    AliasDclNode *slow = node;
    AliasDclNode *fast = node;
    while (1) {
        if ((fast = aliasDclNext(fast)) == NULL)
            return;
        if ((fast = aliasDclNext(fast)) == NULL)
            return;
        slow = aliasDclNext(slow);
        if (slow == fast) {
            errorMsgNode((INode*)node, ErrorCircular,
                "%s is defined in terms of itself, so it names no type.", &node->namesym->namestr);
            node->target = unknownType;
            return;
        }
    }
}

// The declaration at the end of a chain of aliases, or NULL for one not yet bound
INode *aliasDclResolve(INode *node) {
    while (node && node->tag == AliasDclTag) {
        INode *target = ((AliasDclNode*)node)->target;
        // A type alias stands for a type expression, which may name nothing at
        // all -- '&mut Config' is the type itself and is the end of the chain
        node = (target && isNameUseNode(target)) ? ((NameUseNode*)target)->dclnode : target;
    }
    return node;
}

// The global an alias's target is reached through, or NULL for any other binding
INode *aliasDclThrough(INode *node) {
    return (node && node->tag == AliasDclTag) ? ((AliasDclNode*)node)->through : NULL;
}

// The access a use of a global's folded name stands for: 'global.member', with
// the member spelled as the source type names it rather than as the fold renamed
// it, and both nodes positioned where the name was written. Every downstream
// pass then sees the path the author could have written by hand.
struct FnCallNode *aliasDclThroughAccess(AliasDclNode *node, INode *at) {
    VarDclNode *global = (VarDclNode*)node->through;
    NameUseNode *recvr = newNameUseNode(global->namesym);
    inodeLexCopy((INode*)recvr, at);
    recvr->dclnode = (INode*)global;
    FnCallNode *fncall = newFnCallNode((INode*)recvr, 0);
    fncall->methfld = (INode*)newMemberUseNode(((NameUseNode*)node->target)->namesym);
    inodeLexCopy((INode*)fncall, at);
    inodeLexCopy(fncall->methfld, at);
    return fncall;
}
