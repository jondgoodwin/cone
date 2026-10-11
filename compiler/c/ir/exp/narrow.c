/** Flow typing: a name narrowed to one variant of its enum
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <stdio.h>
#include <stdlib.h>

// One variable's knowledge: which variants of its enum it may hold here
typedef struct NarrowFact {
    VarDclNode *var;
    StructNode *enumdcl;    // the closed enum the variable's type is (or points at)
    uint32_t nvariants;
    uint64_t possible;      // bit k: the k-th of enumdcl->derived is possible
    uint16_t depth;         // the loop depth at which the narrowing was made
    INode *cause;           // after the narrowing ended: the change that ended it
    StructNode *was;        // and the variant it had narrowed the name to
    INode *test;            // the test that narrowed it, where one did
    // A reference that other references may share (a 'mut' or 'ro' one) is narrowed by a
    // hidden local that holds the narrowed reference, which the loan walk follows as it
    // follows any narrowed reference: a call that could change the variant while it is
    // still to be used is refused. The local is made when a use first needs it, and
    // declared where the narrowing was made: at the head of the branch, or after the 'if'.
    VarDclNode *holder;
    INode *anchor;          // the branch's block (anchorhead), or the statement the local is declared after
    uint16_t anchorscope;   // the scope the local is declared in
    uint8_t anchorhead;
    uint8_t transient;      // made inside one condition ('s is Circle and s.radius > 3'): no local, a narrowed copy
    uint8_t shared;         // the name is a reference other references may share
} NarrowFact;

struct NarrowEnv {
    uint32_t used;
    NarrowFact facts[1];
};

static NarrowFact *gFacts;
static uint32_t gUsed;
static uint32_t gCap;

// A change of a narrowed name, in the order they were made, which the loops read
typedef struct { VarDclNode *var; INode *cause; } NarrowKill;
static NarrowKill *gKills;
static uint32_t gKillUsed;
static uint32_t gKillCap;

typedef struct { VarDclNode *var; INode *use; } NarrowUse;

typedef struct NarrowFrame {
    struct NarrowFrame *outer;
    NarrowEnv *entry;
    uint32_t killstart;
    NarrowUse *uses;
    uint32_t nuses;
    uint32_t usecap;
    int depth;
} NarrowFrame;
static NarrowFrame *gFrame;
static int gLoopDepth;

// The hidden locals made, not yet declared: the block's statements are being walked
typedef struct { VarDclNode *holder; INode *anchor; int head; } NarrowLazy;
static NarrowLazy *gLazy;
static uint32_t gLazyUsed;
static uint32_t gLazyCap;

static INode *gStmt;            // the statement of a block being type checked

// The blocks being type checked, innermost last: a local can be declared only in one of them
static BlockNode **gOpen;
static uint32_t gOpenUsed;
static uint32_t gOpenCap;
static int gLocalAssume;        // facts being made are those of one condition

static uint64_t narrowAll(uint32_t n) {
    return n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1);
}

static int narrowIsSingle(uint64_t p) {
    return p != 0 && (p & (p - 1)) == 0;
}

static uint32_t narrowBitIndex(uint64_t p) {
    uint32_t k = 0;
    while (!(p & 1)) {
        p >>= 1;
        ++k;
    }
    return k;
}

static NarrowFact *narrowFind(VarDclNode *var) {
    for (uint32_t i = 0; i < gUsed; ++i)
        if (gFacts[i].var == var)
            return &gFacts[i];
    return NULL;
}

static NarrowFact *narrowAdd(VarDclNode *var, StructNode *enumdcl, int shared) {
    if (gUsed == gCap) {
        gCap = gCap ? gCap * 2 : 16;
        gFacts = (NarrowFact *)realloc(gFacts, gCap * sizeof(NarrowFact));
    }
    NarrowFact *fact = &gFacts[gUsed++];
    memset(fact, 0, sizeof(NarrowFact));
    fact->var = var;
    fact->enumdcl = enumdcl;
    fact->nvariants = enumdcl->derived->used;
    fact->possible = narrowAll(fact->nvariants);
    fact->shared = (uint8_t)shared;
    return fact;
}

static void narrowRemove(NarrowFact *fact) {
    *fact = gFacts[--gUsed];
}

// A fact that says nothing is dropped, unless it remembers the change that ended one
static void narrowTidy(NarrowFact *fact) {
    if (fact->possible == narrowAll(fact->nvariants) && fact->cause == NULL)
        narrowRemove(fact);
}

static StructNode *narrowVariantOf(NarrowFact *fact) {
    if (!narrowIsSingle(fact->possible))
        return NULL;
    return (StructNode *)nodesGet(fact->enumdcl->derived, narrowBitIndex(fact->possible));
}

// A fact just made single: a narrowing made here and now
static void narrowFresh(NarrowFact *fact, INode *test) {
    fact->test = test;
    fact->depth = (uint16_t)gLoopDepth;
    fact->cause = NULL;
    fact->was = NULL;
    fact->holder = NULL;
    fact->anchor = NULL;
    fact->transient = (uint8_t)gLocalAssume;
}

// An enum whose variants are all known, and so can be counted
static StructNode *narrowClosedEnum(INode *dcl) {
    if (dcl == NULL || dcl->tag != StructTag)
        return NULL;
    StructNode *strnode = (StructNode *)dcl;
    if (!(strnode->flags & TraitType) || strnode->basetrait != NULL || 0 == (strnode->flags & (HasTagField | SameSize))
        || strnode->derived == NULL || strnode->derived->used == 0 || strnode->derived->used > 64)
        return NULL;
    // An enum laid out as a pointer, null standing for its empty variant, has no tag to
    // read and no variant struct to read the value as
    if (strnode->flags & NullablePtr)
        return NULL;
    return strnode;
}

// A name the writer can say: not the hidden variable a match or a temporary makes
static int narrowIsNamed(VarDclNode *var) {
    return var->namesym != anonName && var->namesym != NULL && (&var->namesym->namestr)[0] != '-';
}

// The closed enum a local or parameter holds, by value or through a borrowed
// reference. A reference that no other reference can change the variant under ('uni',
// 'imm', 'mut1') is narrowed as the name itself; one that others may share is narrowed by
// a hidden local, followed by the loan walk (*shared).
static StructNode *narrowEnumOfVar(VarDclNode *var, int *isref, int *shared) {
    if (var->tag != VarDclTag || var->scope == 0 || !narrowIsNamed(var) || (var->flags & FlagStatic))
        return NULL;
    INode *vtype = var->vtype;
    if (vtype == NULL || vtype == unknownType || vtype == errorType || !isTypeNode(vtype))
        return NULL;
    INode *type = itypeGetTypeDcl(vtype);
    *isref = 0;
    *shared = 0;
    if (type->tag == RefTag) {
        RefNode *ref = (RefNode *)type;
        if (itypeGetTypeDcl(ref->region) != (INode *)borrowRef)
            return NULL;
        if (!(permGetFlags(ref->perm) & MayIntRefSum))
            *shared = 1;
        type = itypeGetTypeDcl(ref->vtexp);
        *isref = 1;
    }
    return narrowClosedEnum(type);
}

// The local or parameter a node names, whole
static VarDclNode *narrowVarOf(INode *node) {
    if (node == NULL || !isNameUseNode(node) || !isExpNode(node))
        return NULL;
    INode *dcl = ((NameUseNode *)node)->dclnode;
    if (dcl == NULL || dcl->tag != VarDclTag)
        return NULL;
    return (VarDclNode *)dcl;
}

static uint32_t narrowIndexOf(StructNode *enumdcl, INode *variant) {
    INode **nodesp;
    uint32_t cnt, k = 0;
    for (nodesFor(enumdcl->derived, cnt, nodesp)) {
        if (*nodesp == variant)
            return k;
        ++k;
    }
    return ~0u;
}

// ---- The environment ----

NarrowEnv *narrowSave(void) {
    if (gUsed == 0)
        return NULL;
    NarrowEnv *env = (NarrowEnv *)memAllocBlk(sizeof(NarrowEnv) + (gUsed - 1) * sizeof(NarrowFact));
    env->used = gUsed;
    memcpy(env->facts, gFacts, gUsed * sizeof(NarrowFact));
    return env;
}

void narrowRestore(NarrowEnv *env) {
    gUsed = 0;
    if (env == NULL)
        return;
    if (env->used > gCap) {
        gCap = env->used + 16;
        gFacts = (NarrowFact *)realloc(gFacts, gCap * sizeof(NarrowFact));
    }
    memcpy(gFacts, env->facts, env->used * sizeof(NarrowFact));
    gUsed = env->used;
}

// What holds where two paths meet: a name may hold any variant either path says it may.
// The change that ended a narrowing on either path is remembered.
NarrowEnv *narrowJoin(NarrowEnv *a, NarrowEnv *b) {
    uint32_t na = a ? a->used : 0;
    uint32_t nb = b ? b->used : 0;
    if (na + nb == 0)
        return NULL;
    NarrowEnv *out = (NarrowEnv *)memAllocBlk(sizeof(NarrowEnv) + (na + nb) * sizeof(NarrowFact));
    out->used = 0;
    for (uint32_t i = 0; i < na; ++i) {
        NarrowFact fact = a->facts[i];
        NarrowFact *other = NULL;
        for (uint32_t j = 0; j < nb && other == NULL; ++j)
            if (b->facts[j].var == fact.var)
                other = &b->facts[j];
        fact.possible = other ? (fact.possible | other->possible) : narrowAll(fact.nvariants);
        if (other && other->depth < fact.depth)
            fact.depth = other->depth;
        if (fact.cause == NULL && other && other->cause) {
            fact.cause = other->cause;
            fact.was = other->was;
        }
        // The local that holds the narrowed reference is good where both paths have it
        if (!other || other->holder != fact.holder)
            fact.holder = NULL;
        if (!other || other->anchor != fact.anchor)
            fact.anchor = NULL;
        if (!other || !other->transient)
            fact.transient = 0;
        if (fact.possible != narrowAll(fact.nvariants) || fact.cause)
            out->facts[out->used++] = fact;
    }
    for (uint32_t j = 0; j < nb; ++j) {
        NarrowFact fact = b->facts[j];
        int found = 0;
        for (uint32_t i = 0; i < na && !found; ++i)
            found = a->facts[i].var == fact.var;
        if (found || fact.cause == NULL)
            continue;
        fact.possible = narrowAll(fact.nvariants);
        fact.holder = NULL;
        fact.anchor = NULL;
        fact.transient = 0;
        out->facts[out->used++] = fact;
    }
    if (out->used == 0)
        return NULL;
    return out;
}

void narrowFnBegin(NarrowFnSave *save) {
    save->env = narrowSave();
    save->depth = gLoopDepth;
    save->frames = gFrame;
    save->lazy = gLazyUsed;
    save->stmt = gStmt;
    gUsed = 0;
    gLoopDepth = 0;
    gFrame = NULL;
    gStmt = NULL;
    gLocalAssume = 0;
}

void narrowFnEnd(NarrowFnSave *save) {
    narrowRestore(save->env);
    gLoopDepth = save->depth;
    gFrame = (NarrowFrame *)save->frames;
    gLazyUsed = save->lazy;
    gStmt = (INode *)save->stmt;
}

// ---- What a condition says ----

static void narrowAssumeVar(VarDclNode *var, StructNode *enumdcl, int shared, uint32_t idx, int truth, INode *test) {
    NarrowFact *fact = narrowFind(var);
    if (fact == NULL)
        fact = narrowAdd(var, enumdcl, shared);
    uint64_t bit = (uint64_t)1 << idx;
    // What the test found holds, whatever was believed before it: a test that
    // contradicts it is of a branch that cannot run, which is no reason to refuse
    // the code in it
    if (truth)
        fact->possible = bit;
    else {
        fact->possible &= ~bit;
        if (fact->possible == 0)
            fact->possible = narrowAll(fact->nvariants);
    }
    // A test made in this pass of a loop is a narrowing made in it
    if (narrowIsSingle(fact->possible))
        narrowFresh(fact, test);
    narrowTidy(fact);
}

static void narrowAssumeCond(INode *cond, int truth) {
    if (cond == NULL || cond == elseCond)
        return;
    switch (cond->tag) {
    case IsTag:
    {
        CastNode *isnode = (CastNode *)cond;
        if ((isnode->flags & FlagMatchValue) || !isExpNode(isnode->exp) || !isTypeNode(isnode->typ)
            || castPatternPending(isnode->typ))
            return;
        VarDclNode *var = narrowVarOf(isnode->exp);
        if (var == NULL)
            return;
        int isref, shared;
        StructNode *enumdcl = narrowEnumOfVar(var, &isref, &shared);
        if (enumdcl == NULL)
            return;
        uint32_t idx = narrowIndexOf(enumdcl, itypeGetDerefTypeDcl(isnode->typ));
        if (idx == ~0u)
            return;
        narrowAssumeVar(var, enumdcl, shared, idx, truth, cond);
        return;
    }
    case NotLogicTag:
        narrowAssumeCond(((LogicNode *)cond)->lexp, !truth);
        return;
    case AndLogicTag:
        if (truth) {
            narrowAssumeCond(((LogicNode *)cond)->lexp, 1);
            narrowAssumeCond(((LogicNode *)cond)->rexp, 1);
        }
        return;
    case OrLogicTag:
        if (!truth) {
            narrowAssumeCond(((LogicNode *)cond)->lexp, 0);
            narrowAssumeCond(((LogicNode *)cond)->rexp, 0);
        }
        return;
    }
}

void narrowAssume(INode *cond, int truth) {
    narrowAssumeCond(cond, truth);
}

// The same inside a condition, for its right operand: a name narrowed there is good for
// that operand alone, so no local is made for it
void narrowAssumeLocal(INode *cond, int truth) {
    gLocalAssume = 1;
    narrowAssumeCond(cond, truth);
    gLocalAssume = 0;
}

// ---- What ends a narrowing ----

static void narrowKillLog(VarDclNode *var, INode *cause) {
    if (gKillUsed == gKillCap) {
        gKillCap = gKillCap ? gKillCap * 2 : 32;
        gKills = (NarrowKill *)realloc(gKills, gKillCap * sizeof(NarrowKill));
    }
    gKills[gKillUsed].var = var;
    gKills[gKillUsed].cause = cause;
    ++gKillUsed;
}

uint32_t narrowKillMark(void) {
    return gKillUsed;
}

// A branch that leaves the loop, or the function, does not carry its changes to the
// loop's next pass; one that continues does
void narrowKillDrop(uint32_t mark) {
    if (mark < gKillUsed)
        gKillUsed = mark;
}

// The name may hold any variant from here; where it was narrowed, the change is
// remembered, to be named if the narrowing is wanted again
static void narrowKillFact(NarrowFact *fact, INode *cause) {
    StructNode *variant = narrowVariantOf(fact);
    if (variant != NULL) {
        fact->cause = cause;
        fact->was = variant;
    }
    fact->possible = narrowAll(fact->nvariants);
    fact->holder = NULL;
    fact->anchor = NULL;
    fact->transient = 0;
    narrowTidy(fact);
}

void narrowKill(VarDclNode *var, INode *cause) {
    NarrowFact *fact = narrowFind(var);
    if (fact == NULL)
        return;
    if (narrowIsSingle(fact->possible))
        narrowKillLog(var, cause);
    narrowKillFact(fact, cause);
}

void narrowAssigned(INode *lval, INode *rval) {
    if (lval == NULL)
        return;
    // '*s = v' through a reference variable replaces the value it points at
    if (lval->tag == DerefTag) {
        VarDclNode *refvar = narrowVarOf(((StarNode *)lval)->vtexp);
        if (refvar != NULL)
            narrowKill(refvar, lval);
        return;
    }
    VarDclNode *var = narrowVarOf(lval);
    if (var == NULL)
        return;
    narrowKill(var, lval);
    // A variant stored is the variant it holds
    int isref, shared;
    StructNode *enumdcl = narrowEnumOfVar(var, &isref, &shared);
    if (enumdcl == NULL || isref || rval == NULL)
        return;
    INode *value = rval;
    while (value->tag == CastTag && !(value->flags & FlagConvert))
        value = ((CastNode *)value)->exp;
    if (!isExpNode(value))
        return;
    uint32_t idx = narrowIndexOf(enumdcl, iexpGetTypeDcl(value));
    if (idx == ~0u)
        return;
    NarrowFact *fact = narrowFind(var);
    if (fact == NULL)
        fact = narrowAdd(var, enumdcl, 0);
    fact->possible = (uint64_t)1 << idx;
    int svlocal = gLocalAssume;
    gLocalAssume = 0;
    narrowFresh(fact, NULL);
    gLocalAssume = svlocal;
}

void narrowDeclared(VarDclNode *var) {
    int isref, shared;
    StructNode *enumdcl = narrowEnumOfVar(var, &isref, &shared);
    if (enumdcl == NULL || isref || var->value == NULL || !isExpNode(var->value))
        return;
    INode *value = var->value;
    while (value->tag == CastTag && !(value->flags & FlagConvert))
        value = ((CastNode *)value)->exp;
    if (!isExpNode(value))
        return;
    uint32_t idx = narrowIndexOf(enumdcl, iexpGetTypeDcl(value));
    if (idx == ~0u)
        return;
    NarrowFact *fact = narrowFind(var);
    if (fact == NULL)
        fact = narrowAdd(var, enumdcl, 0);
    fact->possible = (uint64_t)1 << idx;
    int svlocal = gLocalAssume;
    gLocalAssume = 0;
    narrowFresh(fact, NULL);
    gLocalAssume = svlocal;
}

void narrowBorrowed(INode *place, INode *perm, INode *pointee) {
    if (gUsed == 0 || place == NULL)
        return;
    INode *root = place;
    if (root->tag == DerefTag)
        root = ((StarNode *)root)->vtexp;
    VarDclNode *var = narrowVarOf(root);
    if (var == NULL)
        return;
    NarrowFact *fact = narrowFind(var);
    if (fact == NULL || !(permGetFlags(perm) & MayWrite))
        return;
    // Only a borrow of the whole enum may store another variant in it
    if (itypeGetTypeDcl(pointee) != (INode *)fact->enumdcl)
        return;
    narrowKill(var, place);
}

// ---- The local that holds a narrowed shared reference ----

// The statement of a block now being type checked, so that an 'if' knows whether it is
// one, and what follows it in its block is declared after it
INode *narrowStmtBegin(INode *stmt) {
    INode *prev = gStmt;
    gStmt = stmt;
    return prev;
}

void narrowStmtEnd(INode *prev) {
    gStmt = prev;
}

int narrowIsStmt(INode *node) {
    return node == gStmt;
}

// Narrowings made at the head of a branch, whose condition said the variant: a shared
// reference is held by a local declared at the branch's head
void narrowAnchorArm(BlockNode *arm, uint16_t scope) {
    for (uint32_t i = 0; i < gUsed; ++i) {
        NarrowFact *fact = &gFacts[i];
        if (fact->shared && narrowIsSingle(fact->possible) && fact->holder == NULL && fact->anchor == NULL
            && !fact->transient) {
            fact->anchor = (INode *)arm;
            fact->anchorhead = 1;
            fact->anchorscope = scope;
        }
    }
}

// Narrowings that hold after an 'if', which a branch that left made: declared after it,
// where it is a statement of its block
void narrowAnchorAfter(INode *ifnode, uint16_t scope, int direct) {
    if (!direct)
        return;
    for (uint32_t i = 0; i < gUsed; ++i) {
        NarrowFact *fact = &gFacts[i];
        if (fact->shared && narrowIsSingle(fact->possible) && fact->holder == NULL && fact->anchor == NULL
            && !fact->transient) {
            fact->anchor = ifnode;
            fact->anchorhead = 0;
            fact->anchorscope = scope;
        }
    }
}

static void narrowLazyAdd(VarDclNode *holder, INode *anchor, int head) {
    if (gLazyUsed == gLazyCap) {
        gLazyCap = gLazyCap ? gLazyCap * 2 : 16;
        gLazy = (NarrowLazy *)realloc(gLazy, gLazyCap * sizeof(NarrowLazy));
    }
    gLazy[gLazyUsed].holder = holder;
    gLazy[gLazyUsed].anchor = anchor;
    gLazy[gLazyUsed].head = head;
    ++gLazyUsed;
}

static void narrowLazyRemove(uint32_t i) {
    gLazy[i] = gLazy[--gLazyUsed];
}

// The branch is type checked: declare the locals its uses made, at its head
void narrowInsertHeads(BlockNode *arm) {
    for (uint32_t i = 0; i < gLazyUsed;) {
        if (gLazy[i].head && gLazy[i].anchor == (INode *)arm) {
            nodesInsert(&arm->stmts, (INode *)gLazy[i].holder, 0);
            narrowLazyRemove(i);
        }
        else
            ++i;
    }
}

// The block is type checked: declare the locals made for a narrowing that holds after
// one of its statements, right after it
void narrowInsertAfters(BlockNode *blk) {
    // (the block is done)
    if (gOpenUsed > 0)
        --gOpenUsed;
    if (gLazyUsed == 0 || blk->stmts == NULL)
        return;
    for (uint32_t i = 0; i < gLazyUsed;) {
        if (gLazy[i].head) {
            ++i;
            continue;
        }
        uint32_t at;
        for (at = 0; at < blk->stmts->used && nodesGet(blk->stmts, at) != gLazy[i].anchor; ++at)
            ;
        if (at == blk->stmts->used) {
            ++i;
            continue;
        }
        nodesInsert(&blk->stmts, (INode *)gLazy[i].holder, at + 1);
        narrowLazyRemove(i);
    }
}

void narrowBlockBegin(BlockNode *blk) {
    if (gOpenUsed == gOpenCap) {
        gOpenCap = gOpenCap ? gOpenCap * 2 : 32;
        gOpen = (BlockNode **)realloc(gOpen, gOpenCap * sizeof(BlockNode *));
    }
    gOpen[gOpenUsed++] = blk;
}

// Can the local for this fact still be declared where the narrowing was made? Its block is
// one being checked: the branch the test led to, or the block the if is a statement of
// (not a block it was wrapped in, which is done by the time a name is used after it)
static int narrowAnchorOpen(NarrowFact *fact) {
    for (uint32_t i = 0; i < gOpenUsed; ++i) {
        if (fact->anchorhead) {
            if ((INode *)gOpen[i] == fact->anchor)
                return 1;
        }
        else if (gOpen[i]->stmts) {
            for (uint32_t k = 0; k < gOpen[i]->stmts->used; ++k)
                if (nodesGet(gOpen[i]->stmts, k) == fact->anchor)
                    return 1;
        }
    }
    return 0;
}

// The reference variable's type, pointing at the variant instead of the enum
static RefNode *narrowRefTo(RefNode *held, StructNode *variant) {
    RefNode *narrowed = newRefNode(held->tag);
    *narrowed = *held;
    narrowed->typeinfo = NULL;
    narrowed->vtexp = (INode *)variant;
    return narrowed;
}

// The hidden local '-narrow' for a shared reference narrowed to a variant: it holds the
// narrowed reference, a copy of the variable's, which the loan walk follows
static VarDclNode *narrowMakeHolder(TypeCheckState *pstate, NarrowFact *fact, StructNode *variant, INode *lexnode) {
    VarDclNode *var = fact->var;
    NameUseNode *use = (NameUseNode *)newNameUseFromDclNode((INode *)var, lexnode);
    CastNode *cast = castNarrowRef((INode *)use, narrowRefTo((RefNode *)itypeGetTypeDcl(var->vtype), variant));
    if (fact->test) {
        inodeLexCopy((INode *)use, fact->test);
        inodeLexCopy((INode *)cast, fact->test);
    }
    Name *name = nametblFind("-narrow", 7);
    VarDclNode *holder = newVarDclFull(name, VarDclTag, unknownType, (INode *)immPerm, (INode *)cast);
    inodeLexCopy((INode *)holder, fact->test ? fact->test : lexnode);
    holder->scope = fact->anchorscope;
    varDclTypeCheck(pstate, holder);
    holder->flags |= TypeChecked;
    narrowLazyAdd(holder, fact->anchor, fact->anchorhead);
    return holder;
}

// ---- Naming a member of the narrowed variant ----

static void narrowAt(INode *node, char *buf, size_t size) {
    snprintf(buf, size, "%u:%u", node->linenbr, (uint32_t)(node->srcp - node->linep) + 1);
}

static void narrowAddUse(NarrowFrame *frame, VarDclNode *var, INode *use) {
    if (frame->nuses == frame->usecap) {
        frame->usecap = frame->usecap ? frame->usecap * 2 : 8;
        frame->uses = (NarrowUse *)realloc(frame->uses, frame->usecap * sizeof(NarrowUse));
    }
    frame->uses[frame->nuses].var = var;
    frame->uses[frame->nuses].use = use;
    ++frame->nuses;
}

void narrowReceiver(TypeCheckState *pstate, FnCallNode *node) {
    if (gUsed == 0)
        return;
    if (node->methfld == NULL || !isNameUseNode(node->methfld) || (node->flags & (FlagOperator | FlagIndex)))
        return;
    VarDclNode *var = narrowVarOf(node->objfn);
    if (var == NULL)
        return;
    NarrowFact *fact = narrowFind(var);
    if (fact == NULL)
        return;
    NameUseNode *use = (NameUseNode *)node->objfn;
    int isref, shared;
    if (narrowEnumOfVar(var, &isref, &shared) != fact->enumdcl)
        return;
    // The use is of the variable as its enum: not one already typed as a variant
    INode *usetype = itypeGetTypeDcl(use->vtype);
    if (isref) {
        if (usetype->tag != RefTag || itypeGetTypeDcl(((RefNode *)usetype)->vtexp) != (INode *)fact->enumdcl)
            return;
    }
    else if (usetype != (INode *)fact->enumdcl)
        return;
    Name *member = ((NameUseNode *)node->methfld)->namesym;
    // What the enum has, the enum's use finds: only a member of the variant alone is
    // reached by narrowing
    if (iNsTypeFindFnField((INsTypeNode *)fact->enumdcl, member) != NULL)
        return;
    StructNode *variant = narrowVariantOf(fact);
    if (variant == NULL) {
        // The name was a variant, and something has changed it since
        if (fact->cause != NULL && fact->was != NULL
            && iNsTypeFindFnField((INsTypeNode *)fact->was, member) != NULL) {
            char at[40];
            narrowAt(fact->cause, at, sizeof(at));
            errorMsgNode(node->objfn, ErrorNarrowStale,
                "`%s` is not known to be %s here, and `%s` is a member of %s: `%s` was changed at %s. Test it again with 'is', or bind it with 'if imm x %s = %s'.",
                &var->namesym->namestr, &fact->was->namesym->namestr, &member->namestr, &fact->was->namesym->namestr,
                &var->namesym->namestr, at, &fact->was->namesym->namestr, &var->namesym->namestr);
            node->vtype = errorType;
        }
        return;
    }
    if (iNsTypeFindFnField((INsTypeNode *)variant, member) == NULL)
        return;
    if (isref && shared) {
        // The loan walk follows the narrowed reference a local holds. Where no local
        // can stand (the narrowing was made in a condition, or by a statement that is
        // not its block's), the name is not narrowed.
        if (fact->holder == NULL && fact->anchor != NULL && narrowAnchorOpen(fact))
            fact->holder = narrowMakeHolder(pstate, fact, variant, node->objfn);
        if (fact->holder != NULL) {
            NameUseNode *held = (NameUseNode *)newNameUseFromDclNode((INode *)fact->holder, node->objfn);
            node->objfn = (INode *)held;
        }
        else if (fact->transient)
            node->objfn = (INode *)castNarrowRef(node->objfn, narrowRefTo((RefNode *)usetype, variant));
        else
            return;
    }
    else if (isref) {
        use->vtype = (INode *)narrowRefTo((RefNode *)usetype, variant);
        use->flags |= FlagNarrowed;
    }
    else {
        use->vtype = (INode *)variant;
        use->flags |= FlagNarrowed;
    }
    // A narrowing from before the loop this is in
    if (fact->depth < gLoopDepth) {
        for (NarrowFrame *frame = gFrame; frame != NULL && frame->depth > fact->depth; frame = frame->outer)
            narrowAddUse(frame, var, (INode *)use);
    }
}

// ---- Loops ----

void narrowLoopEnter(void) {
    NarrowFrame *frame = (NarrowFrame *)memAllocBlk(sizeof(NarrowFrame));
    memset(frame, 0, sizeof(NarrowFrame));
    frame->outer = gFrame;
    frame->entry = narrowSave();
    frame->killstart = gKillUsed;
    frame->depth = ++gLoopDepth;
    gFrame = frame;
}

void narrowLoopExit(void) {
    NarrowFrame *frame = gFrame;
    if (frame == NULL)
        return;
    gFrame = frame->outer;
    --gLoopDepth;
    for (uint32_t i = 0; i < frame->nuses; ++i) {
        for (uint32_t k = frame->killstart; k < gKillUsed; ++k) {
            if (gKills[k].var != frame->uses[i].var)
                continue;
            char at[40];
            narrowAt(gKills[k].cause, at, sizeof(at));
            errorMsgNode(frame->uses[i].use, ErrorNarrowLoop,
                "`%s` is narrowed to one variant from before this loop, but the loop changes it at %s and comes back here: the narrowing does not hold on the next pass. Test it again inside the loop.",
                &frame->uses[i].var->namesym->namestr, at);
            break;
        }
    }
    // What was known going in, less what the loop changed
    narrowRestore(frame->entry);
    for (uint32_t k = frame->killstart; k < gKillUsed; ++k) {
        NarrowFact *fact = narrowFind(gKills[k].var);
        if (fact != NULL)
            narrowKillFact(fact, gKills[k].cause);
    }
    free(frame->uses);
}
