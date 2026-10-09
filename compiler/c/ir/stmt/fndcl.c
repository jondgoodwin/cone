/** Handling for function/method declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"
#include "../../shared/timer.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

// Create a new function declaraction node
FnDclNode *newFnDclNode(Name *namesym, uint16_t flags, INode *type, INode *val) {
    FnDclNode *node;
    newNode(node, FnDclNode, FnDclTag);
    node->flags = flags;
    node->vtype = type;
    node->namesym = namesym;
    node->overloadsym = NULL;
    node->value = val;
    node->llvmvar = NULL;
    dclInfoInit(&node->dclinfo);
    node->genericinfo = NULL;
    node->where = NULL;
    node->compute[0] = node->compute[1] = node->compute[2] = 0;
    return node;
}

// Create a new overloaded function/method declaration node
FnOverloadDclNode *newFnOverloadDclNode(Name *namesym) {
    FnOverloadDclNode *node;
    newNode(node, FnOverloadDclNode, FnOverloadDclTag);
    node->namesym = namesym;
    node->overloads = newNodes(2);
    return node;
}

// Append a concrete declaration to an overload set's ordered candidates.
// The set is a method set when any of its candidates is a method, which is
// what lets an unqualified use be rewritten to 'self.name'.
//
// The name's visibility is its candidates': the first candidate declares it,
// and every later one must agree. A private candidate may not join a pub name,
// since the name would make it reachable from outside its owner and a symbol
// that is private and reachable has no sound linkage; and a pub candidate may
// not join a private name, which would hide what it declares visible. A
// compiler-defined intrinsic is not a symbol at all: it counts as pub for the
// name it joins, which is what lets the core types hide '_neg' behind a pub
// '-', and it is exempt from agreeing.
void fnOverloadDclAdd(FnOverloadDclNode *ovlnode, FnDclNode *fnnode) {
    int intrinsic = fnnode->value && fnnode->value->tag == IntrinsicTag;
    int ispub = intrinsic || (fnnode->flags & FlagPub) != 0;
    if (ovlnode->overloads->used == 0) {
        if (ispub)
            ovlnode->flags |= FlagPub;
    }
    else if (!intrinsic && ispub != ((ovlnode->flags & FlagPub) != 0)) {
        errorMsgNode((INode*)fnnode, ErrorPrivOverload, ispub
            ? "%s is pub, so it may not join the private overload name %s; make both pub or neither."
            : "%s is private, so it may not join the pub overload name %s; make both pub or neither.",
            &fnnode->namesym->namestr, &ovlnode->namesym->namestr);
        return;
    }
    nodesAdd(&ovlnode->overloads, (INode*)fnnode);
    ovlnode->flags |= fnnode->flags & FlagMethFld;
}

// The copy of a function/method declaration, before its signature and body are
// copied: they still point at the original's. Split from filling it in so a
// generic type's clone can bind every member's copy before any body that may
// name one is copied (cloneStructNode).
FnDclNode *cloneFnDclShell(FnDclNode *oldfn) {
    FnDclNode *newnode = memAllocBlk(sizeof(FnDclNode));
    memcpy(newnode, oldfn, sizeof(FnDclNode));
    // A clone is unchecked however far along the node it was copied from got.
    // memcpy carries the type check marks with everything else, and a clone that
    // kept them would be skipped by the guard in inodeTypeCheck.
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    newnode->dclinfo.facts &= 0xffff - DclBodyTyped;
    // A generic method copied with its type -- into a generic type's instance,
    // or a trait's default into an implementer -- is still generic there, with
    // its own instances. It shares the original's type parameters, so its
    // signature is the original's: a trait's requirement and an implementer's
    // copy of its default name the same T. Instantiating a generic makes a
    // copy that is not generic (genericClone).
    if (oldfn->genericinfo) {
        newnode->genericinfo = newGenericInfo();
        newnode->genericinfo->parms = oldfn->genericinfo->parms;
    }
    return newnode;
}

// Copy the original's signature and body into its shell.
// A copy that is still generic keeps its type parameters, and a use of one in
// what is copied stays a use of it rather than a substitution. Each is hooked
// to itself for the copy, as a macro's copy hooks its own parameters
// (cloneMacroDclNode): the type parameters this clone substitutes are the
// enclosing generic's, never these, and a name hooked to nothing would be
// read as a substitution of nothing.
void cloneFnDclFill(CloneState *cstate, FnDclNode *newnode, FnDclNode *oldfn) {
    uint32_t dclpos = cloneDclPush();
    int generic = newnode->genericinfo != NULL;
    if (generic) {
        nametblHookPush();
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(newnode->genericinfo->parms, cnt, nodesp))
            nametblHookNode(((GenVarDclNode*)*nodesp)->namesym, *nodesp);
    }
    newnode->vtype = cloneNode(cstate, oldfn->vtype);
    newnode->value = cloneNode(cstate, oldfn->value);
    // A clause on an enclosing type's parameter is substituted, one on this
    // function's own stays a use of it, for its instances to evaluate
    if (oldfn->where)
        newnode->where = cloneNodes(cstate, oldfn->where);
    if (generic)
        nametblHookPop();
    cloneDclPop(dclpos);
}

// Return a clone of a function/method declaration
INode *cloneFnDclNode(CloneState *cstate, FnDclNode *oldfn) {
    FnDclNode *newnode = cloneFnDclShell(oldfn);
    cloneFnDclFill(cstate, newnode, oldfn);
    return (INode*)newnode;
}

// Serialize a function node
void fnDclPrint(FnDclNode *node) {
    if (node->namesym)
        inodeFprint("fn %s", &node->namesym->namestr);
    else
        inodeFprint("fn");
    if (node->genericinfo)
        genericInfoPrint(node->genericinfo);
    dclInfoPrint((INode*)node);
    if (node->overloadsym)
        inodeFprint(" overload %s ", &node->overloadsym->namestr);
    inodePrintNode(node->vtype);
    if (node->value) {
        inodeFprint(" {} ");
        if (node->value->tag == BlockTag)
            inodePrintNL();
        inodePrintNode(node->value);
    }
}

// Serialize an overloaded function/method declaration node.
// Only each candidate's concrete name and signature are printed, as each
// candidate is separately printed by the module or type that owns it.
void fnOverloadDclPrint(FnOverloadDclNode *node) {
    inodeFprint("overload %s", &node->namesym->namestr);
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(node->overloads, cnt, nodesp)) {
        FnDclNode *candidate = (FnDclNode *)*nodesp;
        inodeFprint(" %s ", candidate->namesym? &candidate->namesym->namestr : "");
        inodePrintNode(candidate->vtype);
    }
}

// Whether an importer expands this function's body in its own object rather than
// calling a symbol: an inline or generic function, an intrinsic (whose meaning,
// or fallback body, is expanded at each call), a trait's default (cloned
// into each implementer), a module trait's default (cloned into each conforming
// module), and any method of a generic type (cloned into each instance).
// 'typenode' is the type or module trait whose braces declare it, or NULL.
int fnDclIsExpanded(FnDclNode *fndclnode, INode *typenode) {
    if ((fndclnode->flags & FlagInline) || fndclnode->genericinfo
        || (fndclnode->dclinfo.facts & DclIntrinsic))
        return 1;
    if (typenode && typenode->tag == ModTraitTag)
        return 1;
    if (typenode && typenode->tag == StructTag
        && (((StructNode*)typenode)->genericinfo || (typenode->flags & TraitType)))
        return 1;
    return 0;
}

// Resolve all names in a function
void fnDclNameRes(NameResState *nstate, FnDclNode *fndclnode) {
    INode **nodesp;
    uint32_t cnt;

    // What an expanded body names is marked by nameUseNameRes, so a library
    // compile can give it a symbol an importer links against. A function nested
    // in such a body is part of it, so it inherits the expander.
    INode *svexpander = nstate->expander;
    if (fnDclIsExpanded(fndclnode, nstate->typenode))
        nstate->expander = (INode*)fndclnode;

    nametblHookPush();
    // Resolve generic parameters inside the hooked context. Resolving one hooks
    // it, so doing it before the push would bind it in the enclosing scope and
    // the matching pop would never remove it.
    if (fndclnode->genericinfo)
        genericParmsNameRes(nstate, fndclnode->genericinfo->parms);
    // Its constraints: a generic function's are requirements on its own type
    // parameters, and a generic type's method's are conditions on the type's,
    // for the method to exist. A function that is neither has no parameter for
    // a clause to name.
    INode *owner = nstate->typenode;
    if (fndclnode->genericinfo
        || (owner && owner->tag == StructTag && ((StructNode*)owner)->genericinfo))
        genericConstraintsNameRes(nstate, fndclnode->genericinfo ? fndclnode->genericinfo->parms : NULL,
            &fndclnode->where);
    else if (fndclnode->where) {
        errorMsgNode(nodesGet(fndclnode->where, 0), ErrorWhereNoParms,
            "%s has no type parameters, nor is it a member of a generic type, so a 'where' clause has nothing to constrain.",
            fndclnode->namesym ? &fndclnode->namesym->namestr : "This function");
        fndclnode->where = NULL;
    }
    // A lifetime bound, '[T + 'a]', is on one of its own type parameters
    lifeBoundsNameRes(fndclnode, owner);
    // A parameter's default value is expanded where the function is called
    // (fnSigNameRes)
    INode *svsigfn = nstate->sigfn;
    nstate->sigfn = (INode*)fndclnode;
    inodeNameRes(nstate, &fndclnode->vtype);
    nstate->sigfn = svsigfn;

    if (fndclnode->value) {

        uint16_t oldscope = nstate->scope;
        nstate->scope = 1;

        // Hook function's parameters into global fndclnode table
        // so that when we walk the function's logic, parameter names are resolved
        FnSigNode *fnsig = (FnSigNode*)fndclnode->vtype;
        for (nodesFor(fnsig->parms, cnt, nodesp))
            nametblHookNode(((VarDclNode *)*nodesp)->namesym, *nodesp);

        inodeNameRes(nstate, &fndclnode->value);

        nstate->scope = oldscope;
    }

    // An intrinsic is checked against the registry once its types are bound,
    // and given its meaning or its fallback body
    if (fndclnode->dclinfo.facts & DclIntrinsic)
        intrinsicDclNameRes(fndclnode);
    else
        sliceEqDclNameRes(fndclnode);

    nametblHookPop();
    nstate->expander = svexpander;
}

// Does this signature's first parameter, 'self', take '&new'?
static int fnDclHasNewSelf(FnDclNode *fn) {
    FnSigNode *sig = (FnSigNode*)fn->vtype;
    if (sig == NULL || sig->tag != FnSigTag || sig->parms->used == 0)
        return 0;
    VarDclNode *self = (VarDclNode*)nodesGet(sig->parms, 0);
    RefNode *selftype = (RefNode*)self->vtype;
    return self->namesym == selfName && selftype->tag == RefTag && selftype->perm
        && itypeGetTypeDcl(selftype->perm) == (INode*)newPerm;
}

int fnDclIsInit(FnDclNode *fn) {
    return (fn->flags & FlagMethFld) && fnDclHasNewSelf(fn);
}

// A struct's function named 'init', or joining the overload name 'init', is an
// initializer, and it fills its value in place: 'self &new', a reference to
// memory that holds no value yet, and nothing returned. '&new' marks nothing
// else (the parameter's own check refuses it elsewhere, refTypeCheck). A
// module's 'init' is its own, and not one of these. Returns 0 when the
// declaration is refused.
static int fnDclInitCheck(TypeCheckState *pstate, FnDclNode *fnnode) {
    INode *owner = pstate->typenode;
    int named = owner && owner->tag == StructTag
        && (fnnode->namesym == initMethodName || fnnode->overloadsym == initMethodName);
    int newself = fnDclHasNewSelf(fnnode);
    if (!named && !newself)
        return 1;
    if (!named) {
        errorMsgNode((INode*)fnnode, ErrorPermNew,
            "'self &new' is the self of a struct's init, which fills its value in place: 'fn init(self &new, ...)'.");
        return 0;
    }
    if (!newself) {
        errorMsgNode((INode*)fnnode, ErrorInitDcl,
            "An init fills its value in place, through a reference to memory that holds no value yet: 'fn init(self &new, ...)'.");
        return 0;
    }
    INode *rettype = itypeGetTypeDcl(((FnSigNode*)fnnode->vtype)->rettype);
    if (rettype->tag != VoidTag) {
        errorMsgNode((INode*)fnnode, ErrorInitDcl,
            "An init returns nothing: the value it builds is the one it writes through self.");
        return 0;
    }
    return 1;
}

// Syntactic sugar: Turn last statement implicit returns into explicit returns
void fnImplicitReturn(INode *rettype, BlockNode *blk) {
    INode *laststmt;
    // An empty body returns where the body is, which is where a diagnostic
    // about what it returns belongs
    if (blk->stmts->used == 0) {
        INode *nil = (INode*)newNilLitNode();
        inodeLexCopy(nil, (INode*)blk);
        nodesAdd(&blk->stmts, (INode*)newReturnNodeExp(nil));
    }
    laststmt = nodesLast(blk->stmts);
    // A function returning 'Never' hands its last expression to a 'return' as
    // one returning a value does, and returnTypeCheck holds it to a call that
    // does not return either
    int never = itypeIsNever(rettype);
    if (rettype->tag == VoidTag && !never) {
        if (laststmt->tag != ReturnTag)
            nodesAdd(&blk->stmts, (INode*)newReturnNodeExp((INode*)newNilLitNode()));
    }
    else {
        // Inject return in front of expression
        if (isExpOrMacroNode(laststmt)) {
            BreakRetNode *retnode = newReturnNodeExp(laststmt);
            nodesLast(blk->stmts) = (INode*)retnode;
        }
        else if (laststmt->tag != ReturnTag) {
            if (never)
                errorMsgNode(laststmt, ErrorNeverReturns,
                    "This function returns Never, so it must end in a call that does not return, such as 'panic(...)'. This statement is its last, and the function would return after it.");
            else
                errorMsgNode(laststmt, ErrorNoRet, "A return value is expected but this statement cannot give one.");
        }
    }
}

// ---- Compute entry points ---------------------------------------------------
//
// A compute entry point, 'fn @compute(64) bake(inv Invocation, parts &Array[Part],
// out &mut Array[f32])', is a kernel's interface. Its parameters are what a
// dispatch binds: core's Invocation, at most once, which the GPU fills from
// the invocation's built-ins; each slice one storage buffer, read-only for
// '&Array[T]' and read-write for '&mut Array[T]'; and each struct taken by value one
// small read-only storage buffer (WebGPU has no push constants, and a
// uniform buffer's 16-byte array stride is not Cone's layout). It returns
// nothing. What a buffer holds is shared with the CPU byte for byte, so it is
// what Cone and WebGPU lay out alike: 32-bit numbers, and structs and fixed
// arrays of them. The same rules hold on every target, so that the CPU, which
// calls the function in a loop, and the GPU agree on one source. Generation
// makes the kernel (genllvm/genlgpu.c).

// The buffers a kernel binds: WebGPU's eight storage buffers a stage, less
// the one the kernel's failed checks are recorded in (genlgpu.c)
#define ComputeMaxBuffers 7

// Whether a struct type is an enum, a trait, or one of an enum's variants
static int fnDclComputeTagged(INode *dcl) {
    StructNode *strnode = (StructNode *)dcl;
    return (strnode->flags & (EnumType | HasTagField | TraitType))
        || (strnode->basetrait && (itypeGetTypeDcl(strnode->basetrait)->flags & (EnumType | HasTagField)));
}

// Why 'type', reached through 'path' from the parameter, may not be in a
// buffer, or NULL when it may: 'path' names the field or element at fault. An
// Atomic[u32] or Atomic[i32] is its number, so a buffer of them is a buffer of
// numbers each changed only by atomic operations; WebGPU's atomic<u32> is the
// same four bytes. A '@workgroup' global is held to the same rule
// (varDclTypeCheck).
const char *fnDclComputeData(INode *type, char *path, size_t size) {
    INode *dcl = itypeGetTypeDcl(type);
    switch (dcl->tag) {
    case IntNbrTag:
    case UintNbrTag:
    case FloatNbrTag: {
        unsigned bits = ((NbrNode *)dcl)->bits;
        // Its width is each target's own, so the two sides would disagree
        if (dcl == (INode *)usizeType || dcl == (INode *)isizeType)
            return "a pointer-sized number, as wide as each target's addresses, so not the same on both sides";
        if (bits == 32)
            return NULL;
        if (bits == 1)
            return "a bool, which WebGPU cannot share";
        static char why[128];
        snprintf(why, sizeof(why), "%s, a number of %u bits, which WebGPU cannot share", itypeName(dcl), bits);
        return why;
    }
    case ArrayTag: {
        size_t used = strlen(path);
        if (used + 3 < size)
            strcat(path, "[]");
        return fnDclComputeData(arrayElemType(dcl), path, size);
    }
    case StructTag: {
        StructNode *strnode = (StructNode *)dcl;
        if (fnDclComputeTagged(dcl))
            return "an enum, an Option or a trait, whose tag and layout are Cone's own";
        if (strnode->flags & DeclaredOpaque)
            return "an opaque type, whose layout is unknown";
        size_t used = strlen(path);
        INode **nodesp;
        uint32_t cnt;
        for (nodelistFor(&strnode->fields, cnt, nodesp)) {
            FieldDclNode *field = (FieldDclNode *)*nodesp;
            if (field->tag != FieldDclTag || field->namesym == NULL)
                continue;
            path[used] = '\0';
            if (used + strlen(&field->namesym->namestr) + 2 < size) {
                strcat(path, ".");
                strcat(path, &field->namesym->namestr);
            }
            const char *why = fnDclComputeData(field->vtype, path, size);
            if (why)
                return why;
        }
        path[used] = '\0';
        return NULL;
    }
    case RefTag:
    case ArrayRefTag:
    case VirtRefTag:
    case PtrTag:
        return "a reference, slice or pointer: an address means nothing on the other side";
    default:
        return "not a 32-bit number, nor a struct or fixed array of them";
    }
}

// Hold a compute entry point to the rules above. Its size, and where
// '@compute' may be written, the parser checked.
static void fnDclComputeCheck(FnDclNode *fnnode) {
    FnSigNode *sig = (FnSigNode *)fnnode->vtype;
    if (itypeGetTypeDcl(sig->rettype)->tag != VoidTag)
        errorMsgNode(sig->rettype, ErrorComputeSig,
            "An entry point returns nothing: what a kernel makes, it writes into a slice it was given, '&mut Array[T]'.");
    int invocations = 0, buffers = 0;
    INode **nodesp;
    uint32_t cnt;
    for (nodesFor(sig->parms, cnt, nodesp)) {
        VarDclNode *parm = (VarDclNode *)*nodesp;
        INode *dcl = itypeGetTypeDcl(parm->vtype);
        char path[256];
        snprintf(path, sizeof(path), "%s", &parm->namesym->namestr);
        INode *data;
        if (invocationIsCore(parm->vtype)) {
            if (++invocations > 1)
                errorMsgNode((INode *)parm, ErrorComputeSig,
                    "An entry point takes Invocation at most once: one invocation is running.");
            continue;
        }
        else if (dcl->tag == ArrayRefTag) {
            strcat(path, "[]");
            data = ((RefNode *)dcl)->vtexp;
        }
        else if (dcl->tag == StructTag && !fnDclComputeTagged(dcl)) {
            data = parm->vtype;
        }
        else {
            errorMsgNode((INode *)parm, ErrorComputeSig,
                "An entry point's parameter '%s' is %s; it may be core's Invocation, a slice, '&Array[T]' read or '&mut Array[T]' written, or a struct taken by value, each slice and struct a buffer the dispatch binds.",
                &parm->namesym->namestr, itypeName(parm->vtype));
            continue;
        }
        if (++buffers == ComputeMaxBuffers + 1)
            errorMsgNode((INode *)parm, ErrorComputeSig,
                "An entry point binds at most %d buffers, its slices and structs: WebGPU's 8 storage buffers a stage, one of them recording the kernel's failed checks.",
                ComputeMaxBuffers);
        const char *why = fnDclComputeData(data, path, sizeof(path));
        if (why)
            errorMsgNode((INode *)parm, ErrorComputeData,
                "'%s' is %s. A buffer holds what the CPU and the GPU lay out alike: 32-bit numbers (i32, u32, f32), and structs and fixed arrays of them.",
                path, why);
    }
}

// Type checking a function's logic does more than you might think:
// - Turn implicit returns into explicit returns
// - Perform type checking for all statements
// - Perform data flow analysis on variables and references
void fnDclTypeCheck(TypeCheckState *pstate, FnDclNode *fnnode) {
    // Wait until a generic function is instantiated before type checking
    if (fnnode->genericinfo)
        return;

    // Data flow runs only on a function this pass left well typed. Remember the
    // error count on the way in, so that test is about this function alone: an
    // earlier failure elsewhere in the compile must not silence immutability,
    // move and lifetime checking for every function that follows it.
    int errorsOnEntry = errors;

    // Rule 3: the signature is established before anything that could call this
    // function is checked. Checking it can lay out a type for the first time --
    // 'Option[T]' returned by a method of an instance is instantiated here -- and
    // the last layout to finish works the members queue, which could hold the
    // type whose method calls this one. Held as a layout in flight, the queue is
    // worked only once the whole signature is in place, so such a call reads the
    // return type rather than the generic call it was written as. See
    // compiler/c/doc/phases/type-check.md, "Layout before members".
    // A signature holds nothing by value, so a reference in it is not checked
    // as inside whatever layout this function was demanded from: what it points
    // at is laid out here, before the signature's lifetimes read it.
    structLayoutEnter();
    uint32_t valuebase = structValueHold();
    uint32_t target = structTargetSuspend();
    itypeTypeCheck(pstate, &fnnode->vtype);
    structTargetResume(target);
    structValueRelease(valuebase);
    int sigfailed = errors != errorsOnEntry;
    structLayoutExit();

    // A body is not checked against a signature that failed: every use of the
    // types that check was supposed to establish would report again, naming
    // nothing the author can act on. This is the same shape as the flow gate
    // below -- the count this call entered with, so that it is about this
    // declaration alone and not about whatever failed elsewhere. Whatever the
    // queue checked on the way out of the signature belongs to other
    // declarations, so the count starts again after it.
    if (sigfailed)
        return;
    if (fnDclIsCompute(fnnode))
        fnDclComputeCheck(fnnode);
    errorsOnEntry = errors;

    if (!fnDclInitCheck(pstate, fnnode))
        return;

    // An intrinsic's instance is judged for the type it acts on whether it is
    // lowered or runs its fallback body, which is not checked at a type it was
    // never written for
    if ((fnnode->dclinfo.facts & DclIntrinsic) && !intrinsicClassCheck(fnnode))
        return;

    // A declared intrinsic has no body to check: its meaning is the registry's,
    // and what is left to check is the type it was instantiated for
    if (intrinsicIsDeclared(fnnode)) {
        intrinsicDclTypeCheck(pstate, fnnode);
        return;
    }

    // No need to type check function body if no body or is a default method of a
    // trait, or a module trait's default: each is checked in the copy its
    // implementer or conforming module owns, where its names are that one's
    if (!fnnode->value
        || ((fnnode->flags & FlagMethFld) && pstate->typenode->tag == StructTag && (pstate->typenode->flags & TraitType))
        || (pstate->typenode && pstate->typenode->tag == ModTraitTag))
        return;

    // Ensure self parameter on a method is (reference to) its enclosing type
    if (fnnode->flags & FlagMethFld) {
        INode *selfparm = nodesGet(((FnSigNode *)(fnnode->vtype))->parms, 0);
        // An 'Array[T]' method's self is the slice '&Array[T]', the one borrow
        // of the body, whose target is the element
        INode *selfdcl = iexpGetTypeDcl(selfparm);
        int selfisbody = selfdcl->tag == ArrayRefTag && pstate->typenode->tag == StructTag
            && itypeIsArrayBody((INode*)pstate->typenode)
            && itypeIsSame(((RefNode*)selfdcl)->vtexp, itypeLenBodyElem((INode*)pstate->typenode));
        if (!selfisbody && iexpGetDerefTypeDcl(selfparm) != pstate->typenode)
            errorMsgNode((INode*)fnnode, ErrorInvType, "self parameter for a method must match, or be a reference to, its type");
    }

    // Syntactic sugar: Turn implicit returns into explicit returns
    fnImplicitReturn(((FnSigNode*)fnnode->vtype)->rettype, (BlockNode *)fnnode->value);

    // Type check/inference of the function's logic.
    //
    // Rule 8: this declaration may have been reached by demand, from the middle
    // of some other function's body, so the walk context describes somewhere
    // else. Saving and resetting all three is what makes analyzing a
    // declaration independent of where it was analyzed from. Scope 1 is the
    // signature's, matching what fnDclNameRes sets, so the body's own block is
    // scope 2. The declaration whose initializer may extend a temporary is the
    // other function's: left open, a borrow of a temporary here would make it a
    // hidden local of that function's block (vardcl.c, varDclExtendTemp).
    FnDclNode *svFn = pstate->fn;
    uint16_t svScope = pstate->scope;
    VarDclExtend *svExtend = pstate->extend;
    pstate->fn = fnnode;
    pstate->scope = 1;
    pstate->extend = NULL;
    LifeBrandSave svBrands;
    lifeBrandFnBegin((FnSigNode *)fnnode->vtype, &svBrands);
    inodeTypeCheck(pstate, &fnnode->value, noCareType);
    lifeBrandFnEnd(&svBrands);
    pstate->scope = svScope;
    pstate->fn = svFn;
    pstate->extend = svExtend;
    if (errors == errorsOnEntry)
        fnnode->dclinfo.facts |= DclBodyTyped;

    // An inline body is generated in each caller as a block whose value is the
    // call's, its returns breaking out of it with that value. Every path ends in
    // a return, so the block infers no type of its own; it has the function's
    // return type, which is the phi's type where several returns converge.
    if (fnnode->flags & FlagInline)
        ((BlockNode *)fnnode->value)->vtype = ((FnSigNode *)fnnode->vtype)->rettype;

    // Immediately perform the data flow pass for this function
    // We run data flow separately as it requires type info which is inferred bottoms-up
    // Skip it when this function's own signature or body did not type check, as
    // flow analysis relies on the types that check was supposed to establish.
    if (errors != errorsOnEntry)
        return;
    FlowState fstate;
    flowStateInit(&fstate, (FnSigNode *)fnnode->vtype);
    // An init's 'self &new' holds no value until '*self = value' fills it
    VarDclNode *newself = fnDclIsInit(fnnode) ? (VarDclNode *)nodesGet(((FnSigNode *)fnnode->vtype)->parms, 0) : NULL;
    if (newself)
        newself->flowtempflags |= VarUnfilled;
    // A module's 'init' starts with its module's uninitialized globals holding
    // nothing, as a local does, and must leave each one assigned
    ModuleNode *initmod = modInitOf(fnnode);
    uint16_t *saved = initmod ? modInitFlowBegin(initmod) : NULL;
    // Flow runs inside type check's span, which may itself be nested in another
    // function's, so the timer hands the time back to whichever was running
    size_t svTimer = timerCurrent;
    if (timerFine)
        timerBegin(FlowTimer);
    blockFlow(&fstate, (BlockNode **)&fnnode->value);
    flowCurrent = NULL;
    // A function the gate marked holds a borrow in a way only a walk following
    // each path can check, or has a variable whose state may differ by path,
    // whose drops only such a walk can decide: it is walked again, once
    // blockFlow found no error, for either or both in one walk. On a GPU
    // target every function is walked for loans, whose checks there
    // (flowloan.h, "GPU targets") no trigger of the gate stands for
    // A function holding an 'await' is walked for both, whatever the gates
    // say: the seam's rules are the loan walk's, and whether a variable still
    // holds its value there is drop flags' state (flowpath.c, pwSeam)
    int seams = fstate.awaits != NULL;
    if ((fstate.gate || fstate.dropgate || flowGpu || seams) && errors == errorsOnEntry) {
        int loans = fstate.gate != 0 || flowGpu || seams;
        // A borrow of a shape-changing value freezes what it was reached
        // through, and which types those are is read from their methods, which
        // may still be waiting to be checked. A walk that needs one of them is
        // made when they are, at the end of type check, unless it cannot wait
        // (shapeinfer.h)
        int walknow = 1;
        if (loans) {
            // Checking those methods is analysis, not flow
            if (timerFine)
                timerBegin(svTimer);
            walknow = shapeWalkReady(fnnode, !seams && !initmod);
            // A call made while such a borrow is held could reshape it through
            // another name: which calls do is read from the bodies of the functions
            // called, so those never asked for are checked first (reshape.h)
            if ((fstate.gate & FlowGateShape) || seams)
                shapeDemandCallees(fnnode);
            if (timerFine)
                timerBegin(FlowTimer);
        }
        if (walknow) {
            flowPathWalk(fnnode, loans, fstate.dropgate || seams, seams);
            // A call the walk could not judge, for a body not checked yet: made
            // again at the end of type check, for that verdict alone
            if (flowShapeRetry && errors == errorsOnEntry)
                shapeWalkDefer(fnnode, 0, seams);
        }
        else
            shapeWalkDefer(fnnode, fstate.dropgate != 0, 0);
    }
    // The seams every rule accepted: a message's are split, where the split is
    // built (generation makes its halves); any other is reported not built
    // yet where it stands, with what its continuation would carry
    if (seams && errors == errorsOnEntry)
        awaitSplitOrReport(fnnode, fstate.awaits);
    if (timerFine)
        timerBegin(svTimer);
    flowGateCount(&fstate);
    if (initmod)
        modInitFlowEnd(initmod, saved);
    if (newself)
        newself->flowtempflags &= 0xFFFF - VarUnfilled;
}

// Verify no two candidates of an overload set accept the same parameter signature.
// Candidates are not walked, as each is separately name resolved and type checked
// by the module or type that owns its concrete declaration.
void fnOverloadDclTypeCheck(TypeCheckState *pstate, FnOverloadDclNode *node) {
    INode **nodesp;
    uint32_t cnt;
    uint32_t index = 0;
    for (nodesFor(node->overloads, cnt, nodesp)) {
        FnDclNode *candidate = (FnDclNode *)*nodesp;
        for (uint32_t prior = 0; prior < index; ++prior) {
            FnDclNode *earlier = (FnDclNode *)nodesGet(node->overloads, prior);
            if (fnSigParmsEqual((FnSigNode *)earlier->vtype, (FnSigNode *)candidate->vtype)) {
                errorMsgNode((INode*)candidate, ErrorDupOverload,
                    "%s accepts the same arguments as %s, so overload %s could never choose between them.",
                    &candidate->namesym->namestr, &earlier->namesym->namestr, &node->namesym->namestr);
                break;
            }
        }
        ++index;
    }
}
