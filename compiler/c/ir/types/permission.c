/** Permission Types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <assert.h>
#include <string.h>

// Create a new permission declaration node
PermNode *newPermDclNode(Name *namesym, uint16_t flags) {
    PermNode *node;
    newNode(node, PermNode, PermTag);
    node->namesym = namesym;
    node->llvmtype = NULL;
    iNsTypeInit((INsTypeNode*)node, 1); // May not need members for static types
    node->permflags = flags;
    node->lock = NULL;
    node->held = 0;
    return node;
}

// ---------------------------------------------------------------------------
// Lock permissions
//
// A lock permission is a struct declaring 'LockPermission', written in a
// managed reference's permission slot: 'Arc[Mutex, T]'. Its value is the lock,
// in the allocation's header after the region's part. The reference itself may
// be copied, and crosses threads where the lock and the region both declare
// 'ThreadSafe', but reaches nothing: its flags grant no read and no write. The
// value is reached only by a borrow through it, '&mut *p' or '&*p', which type
// check rewrites to a borrow through a guard (borrowLockPlace): a temporary
// owner of the value, a copy of the reference, whose permission says which of
// the lock's locks it holds. The guard is made by taking the lock, and its
// death -- at the end of the statement, or of the block where a declaration's
// initializer extends it -- gives the lock back before the owner goes.
// ---------------------------------------------------------------------------

// The struct a permission slot names, where it names one
static StructNode *permStructDcl(INode *perm) {
    if (perm == NULL || !isTypeNode(perm))
        return NULL;
    INode *dcl = itypeGetTypeDcl(perm);
    return dcl->tag == StructTag ? (StructNode*)dcl : NULL;
}

int permIsLock(INode *perm) {
    StructNode *strnode = permStructDcl(perm);
    return strnode != NULL && structDeclaresTrait(strnode, lockPermTrait);
}

StructNode *permLockOf(INode *perm) {
    if (perm == NULL || !isTypeNode(perm))
        return NULL;
    INode *dcl = itypeGetTypeDcl(perm);
    if (dcl->tag == PermTag)
        return ((PermNode*)dcl)->lock;
    return dcl->tag == StructTag ? (StructNode*)dcl : NULL;
}

int permHeldKind(INode *perm) {
    if (perm == NULL || !isTypeNode(perm))
        return 0;
    INode *dcl = itypeGetTypeDcl(perm);
    return dcl->tag == PermTag ? ((PermNode*)dcl)->held : 0;
}

int lockPermHasRead(StructNode *lock) {
    return iNsTypeFindFnField((INsTypeNode*)lock, acquireReadMethodName) != NULL;
}

// The guards' permissions made so far, a few per compile
typedef struct {
    StructNode *lock;
    PermNode *held[2];
} LockHelds;
static LockHelds *permHelds = NULL;
static uint32_t permHeldCount = 0;
static uint32_t permHeldMax = 0;

// A guard holding the mutable lock reaches the value as 'uni' does: no other
// path reaches it while the lock is held. One holding a read lock reaches it as
// 'imm' does, since nobody may change it while it is held. Neither may be
// aliased, so the guard moves, and only its one death gives the lock back.
PermNode *permHeld(StructNode *lock, int kind) {
    if (kind == LockHeldRead && !lockPermHasRead(lock))
        kind = LockHeldMut;
    uint32_t i;
    for (i = 0; i < permHeldCount; ++i) {
        if (permHelds[i].lock == lock)
            break;
    }
    if (i == permHeldCount) {
        if (permHeldCount == permHeldMax) {
            uint32_t newmax = permHeldMax ? permHeldMax * 2 : 8;
            LockHelds *helds = (LockHelds *)memAllocBlk(newmax * sizeof(LockHelds));
            if (permHeldCount)
                memcpy(helds, permHelds, permHeldCount * sizeof(LockHelds));
            permHelds = helds;
            permHeldMax = newmax;
        }
        permHelds[i].lock = lock;
        permHelds[i].held[0] = permHelds[i].held[1] = NULL;
        ++permHeldCount;
    }
    PermNode **heldp = &permHelds[i].held[kind - 1];
    if (*heldp == NULL) {
        uint16_t flags = kind == LockHeldMut
            ? MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless
            : MayRead | RaceSafe | MayIntRefSum | IsLockless;
        PermNode *held = newPermDclNode(lock->namesym, flags);
        held->lock = lock;
        held->held = (uint8_t)kind;
        held->flags |= NameResolved | TypeChecked;
        *heldp = held;
    }
    return *heldp;
}

// The static permission a guard's reaches the value as
static PermNode *permHeldBase(PermNode *perm) {
    return perm->held == LockHeldMut ? uniPerm : immPerm;
}

// Create a new permission use node
INode *newPermUseNode(PermNode *permdcl) {
    NameUseNode *perm;
    newNode(perm, NameUseNode, NameUseTag);
    perm->vtype = NULL;
    perm->namesym = permdcl->namesym;
    perm->dclnode = (INode*)permdcl;
    perm->lifeuse = NULL;
    return (INode*)perm;
}

// Serialize a permission node
void permPrint(PermNode *node) {
    inodeFprint("%s ", &node->namesym->namestr);
}

// Get permission's flags. A lock permission grants neither read nor write:
// its reference may be copied, and shared between threads where the lock is
// for them, but reaches its value only by a borrow taking the lock.
int permGetFlags(INode *perm) {
    perm = itypeGetTypeDcl(perm);
    if (perm->tag == StructTag)
        return MayAlias | MayAliasWrite
            | (structDeclaresTrait((StructNode*)perm, threadSafeTrait) ? RaceSafe : 0);
    assert(perm->tag == PermTag);
    return ((PermNode *)perm)->permflags;
}

// Are the permissions the same?
int permIsSame(INode *node1, INode *node2) {
    node1 = itypeGetTypeDcl(node1);
    node2 = itypeGetTypeDcl(node2);
    return node1 == node2;
}

// Will 'from' permission coerce to the target? A lock permission coerces to
// nothing but itself: its reference reaches no value, and its allocation
// holds the lock where a static permission's holds nothing. A guard's coerces
// as the static permission it reaches the value as.
int permMatches(INode *ito, INode *ifrom) {
    PermNode *from = (PermNode *)itypeGetTypeDcl(ifrom);
    PermNode *to = (PermNode *)itypeGetTypeDcl(ito);
    if (to == from)
        return EqMatch;
    if (from->tag != PermTag || to->tag != PermTag || to->held)
        return NoMatch;
    if (from->held)
        from = permHeldBase(from);
    if (to==from || to==opaqPerm)
        return EqMatch;
    if (from == uniPerm &&
        (to == roPerm || to == mutPerm || to == immPerm || to == mut1Perm))
        return EqMatch;
    // An initializer's 'self', once filled, is lent as 'uni' is: a method
    // called on it borrows it for the call. Before it is filled, flow refuses
    // every use of it but the store that fills it (flowNewSelfUse).
    if (from == newPerm &&
        (to == uniPerm || to == roPerm || to == mutPerm || to == immPerm || to == mut1Perm))
        return EqMatch;
    if (to == roPerm &&
        (from == mutPerm || from == immPerm || from == mut1Perm))
        return EqMatch;
    return NoMatch;
}

// Verify that permission init is correctly declared
void permInitTypeCheck(INode *perm) {
    if (perm->tag != StructTag)
        return;

    FnDclNode *initmeth = (FnDclNode*)iTypeFindFnField(perm, initMethodName);
    if (initmeth == NULL) {
        return;
    }
    // Run by allocation on the permission's part of the block, in place, after
    // the region's init (genlallocref). That it takes 'self &new' and returns
    // nothing is every struct's init's rule, reported where the method is
    // checked (fnDclTypeCheck); what is the permission's own is that it takes
    // nothing else.
    if (initmeth->tag != FnDclTag || !fnDclIsInit(initmeth))
        return;
    FnSigNode *initsig = (FnSigNode*)itypeGetTypeDcl(initmeth->vtype);
    if (initsig->parms->used != 1)
        errorMsgNode((INode*)initmeth, ErrorInvType, "Permission init method may not have parameters but self.");
}

// Is this lock method '(self &L)', of any permission, returning nothing? An
// acquiring one ('site') may also take where the borrow is, '(self &L, file
// &[]u8, line u32)', as core's 'panic' does, to name it in a panic.
static int lockPermMethShapeOk(INode *member, StructNode *lock, int site) {
    if (member->tag != FnDclTag || !(member->flags & FlagMethFld))
        return 0;
    FnSigNode *sig = (FnSigNode*)itypeGetTypeDcl(((FnDclNode*)member)->vtype);
    if (itypeGetTypeDcl(sig->rettype)->tag != VoidTag)
        return 0;
    if (sig->parms->used == 3 && site) {
        RefNode *filetype = (RefNode*)itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(sig->parms, 1)));
        if (filetype->tag != ArrayRefTag || itypeGetTypeDcl(filetype->vtexp) != (INode*)u8Type
            || itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(sig->parms, 2))) != (INode*)u32Type)
            return 0;
    }
    else if (sig->parms->used != 1)
        return 0;
    RefNode *selftype = (RefNode*)itypeGetTypeDcl(iexpGetTypeDcl(nodesGet(sig->parms, 0)));
    return selftype->tag == RefTag && selftype->region == borrowRef
        && itypeGetTypeDcl(selftype->vtexp) == (INode*)lock;
}

// One of the methods a borrow calls, where the lock declares it; whether it
// was required and is missing is the caller's to say
static void lockPermCheckMeth(StructNode *lock, Name *name, int site) {
    INode *member = iNsTypeFindFnField((INsTypeNode*)lock, name);
    if (member && !lockPermMethShapeOk(member, lock, site)) {
        char *l = &lock->namesym->namestr;
        if (site)
            errorMsgNode(member, ErrorLockPermShape,
                "A lock permission's %s must be declared 'fn %s(self &%s)', of any permission, or 'fn %s(self &%s, file &[]u8, line u32)', handed where the borrow is: it is handed the lock in the allocation's header, and returns nothing.",
                &name->namestr, &name->namestr, l, &name->namestr, l);
        else
            errorMsgNode(member, ErrorLockPermShape,
                "A lock permission's %s must be declared 'fn %s(self &%s)', of any permission: it is handed the lock in the allocation's header, and returns nothing.",
                &name->namestr, &name->namestr, l);
    }
}

// A lock permission is a struct whose methods a borrow through its reference
// calls: 'acquireMut' as a mutable borrow begins and 'releaseMut' as it ends,
// and, where it declares them, 'acquireRead' and 'releaseRead' for a
// read-only one; without them a read-only borrow takes the mutable lock. Its
// 'init', which an allocation runs on the lock in place, takes nothing else
// (permInitTypeCheck). Run once, at the declaration.
void lockPermCheck(StructNode *node) {
    char *name = &node->namesym->namestr;
    if (node->flags & (TraitType | EnumType)) {
        errorMsgNode((INode*)node, ErrorLockPermShape,
            "Only a struct may declare LockPermission: a lock permission is the lock itself, a value kept in each allocation's header.");
        return;
    }
    int hasacquire = iNsTypeFindFnField((INsTypeNode*)node, acquireMutMethodName) != NULL;
    int hasrelease = iNsTypeFindFnField((INsTypeNode*)node, releaseMutMethodName) != NULL;
    if (!hasacquire || !hasrelease)
        errorMsgNode((INode*)node, ErrorLockPermShape,
            "Lock permission %s must declare 'fn acquireMut(self &%s)' and 'fn releaseMut(self &%s)': a borrow through its reference takes the lock and its end gives it back.",
            name, name, name);
    int hasreadacq = iNsTypeFindFnField((INsTypeNode*)node, acquireReadMethodName) != NULL;
    int hasreadrel = iNsTypeFindFnField((INsTypeNode*)node, releaseReadMethodName) != NULL;
    if (hasreadacq != hasreadrel)
        errorMsgNode((INode*)node, ErrorLockPermShape,
            "Lock permission %s declares one of 'acquireRead' and 'releaseRead': a read-only borrow takes the read lock and gives it back, so it declares both, or neither and takes the mutable lock.",
            name);
    lockPermCheckMeth(node, acquireMutMethodName, 1);
    lockPermCheckMeth(node, releaseMutMethodName, 0);
    lockPermCheckMeth(node, acquireReadMethodName, 1);
    lockPermCheckMeth(node, releaseReadMethodName, 0);
}

void permLockRefused(INode *where, INode *perm, char *what) {
    StructNode *lock = permLockOf(perm);
    errorMsgNode(where, ErrorLockAccess,
        "May not %s through a reference whose permission is the lock %s: its value is reached only by borrowing through it, which takes the lock until the borrow ends ('&mut *r', or '&*r' to read).",
        what, lock ? &lock->namesym->namestr : "it names");
}

// The name a region slot gives, for a message
static char *refLockRegionName(RefNode *node) {
    INode *dcl = isTypeNode(node->region) ? itypeGetTypeDcl(node->region) : NULL;
    return dcl && dcl->tag == StructTag ? &((StructNode*)dcl)->namesym->namestr : "the region";
}

// Which regions a lock fits. A lock is for a value several owners share, so its
// region counts them ('aliasRef' and 'dealiasRef'); a 'Move' region's one owner
// has nothing to lock against. A lock declaring 'ThreadSafe' ('Mutex',
// 'Rwlock') is for owners on several threads, so its region declares it too
// ('Arc'); one that does not ('Rwcell') is for owners on one thread, and its
// region does not ('Rc'). A traced region and a virtual reference take no lock
// yet.
void refLockCheck(RefNode *node) {
    StructNode *lock = permStructDcl(node->perm);
    if (lock == NULL || node->region == borrowRef || !isTypeNode(node->region))
        return;
    char *lockname = &lock->namesym->namestr;
    if (!structDeclaresTrait(lock, lockPermTrait)) {
        errorMsgNode(node->perm, ErrorNotLockPerm,
            "%s is not a permission: a managed reference's first argument is a static permission ('mut', 'imm', ...) or a lock permission, a struct declaring 'is LockPermission'.",
            lockname);
        return;
    }
    char *regname = refLockRegionName(node);
    if (node->tag == VirtRefTag)
        errorMsgNode((INode*)node, ErrorLockRegion,
            "A lock permission is not built on a virtual reference: write %s[%s, T] for a concrete T.",
            regname, lockname);
    else if (regionIsTraced(node->region))
        ;   // Refused with the traced region's other rules (regionTracedJudgeRef)
    else if (regionIsMove(node->region))
        errorMsgNode((INode*)node, ErrorLockRegion,
            "%s has one owner per value, which nothing else shares: a lock is for a value several owners share. 'Arc[Mutex, T]' and 'Arc[Rwlock, T]' share one between threads, 'Rc[Rwcell, T]' within one.",
            regname);
    else if (!regionIsCounted(node->region) || !regionReleaseActs(node->region))
        errorMsgNode((INode*)node, ErrorLockRegion,
            "%s does not count its owners: a lock is for a value whose owners a counting region shares, one declaring 'aliasRef' and 'dealiasRef'.",
            regname);
    else if (structDeclaresTrait(lock, threadSafeTrait) && !regionIsThreadSafe(node->region))
        errorMsgNode((INode*)node, ErrorLockRegion,
            "%s is a lock for owners on several threads, and %s's owners stay on one thread: a lock not declaring ThreadSafe fits it, as 'Rc[Rwcell, T]'; or share the value through a region declaring ThreadSafe, as 'Arc[%s, T]'.",
            lockname, regname, lockname);
    else if (!structDeclaresTrait(lock, threadSafeTrait) && regionIsThreadSafe(node->region))
        errorMsgNode((INode*)node, ErrorLockRegion,
            "%s is a lock for owners on one thread, and %s's owners may be on several at once: a lock declaring ThreadSafe fits it, as '%s[Mutex, T]' or '%s[Rwlock, T]'.",
            lockname, regname, regname, regname);
}
