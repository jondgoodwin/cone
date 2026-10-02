/** Handling for variable declaration nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir.h"

#include <string.h>
#include <assert.h>

// Create a new name declaraction node
VarDclNode *newVarDclNode(Name *namesym, uint16_t tag, INode *perm) {
    VarDclNode *name;
    newNode(name, VarDclNode, tag);
    name->vtype = unknownType;
    name->namesym = namesym;
    name->perm = perm;
    name->value = NULL;
    name->scope = 0;
    name->index = 0;
    name->llvmvar = NULL;
    name->fold = NULL;
    dclInfoInit(&name->dclinfo);
    name->flowtempflags = 0;
    name->flowindex = 0;
    name->flowdepth = 0;
    name->flowtracked = 0;
    name->hollowed = NULL;
    name->hollowall = NULL;
    name->llvmflag = NULL;
    return name;
}

// Create a new name declaraction node
VarDclNode *newVarDclFull(Name *namesym, uint16_t tag, INode *type, INode *perm, INode *val) {
    VarDclNode *name;
    newNode(name, VarDclNode, tag);
    name->vtype = type;
    name->namesym = namesym;
    name->perm = perm;
    name->value = val;
    name->scope = 0;
    name->index = 0;
    name->llvmvar = NULL;
    name->fold = NULL;
    dclInfoInit(&name->dclinfo);
    name->flowtempflags = 0;
    name->flowindex = 0;
    name->flowdepth = 0;
    name->flowtracked = 0;
    name->hollowed = NULL;
    name->hollowall = NULL;
    name->llvmflag = NULL;
    return name;
}

// The copy of a variable declaration, before its type and value are copied: they
// still point at the original's. Split from filling it in so a generic type's
// clone can bind a static's copy before any body that may name it is copied
// (cloneStructNode).
VarDclNode *cloneVarDclShell(VarDclNode *node) {
    VarDclNode *newnode = memAllocBlk(sizeof(VarDclNode));
    memcpy(newnode, node, sizeof(VarDclNode));
    // A clone is unchecked however far along the node it was copied from got.
    // memcpy carries the type check marks with everything else, and a clone that
    // kept them would be skipped by the guard in inodeTypeCheck.
    newnode->flags &= 0xffff - (TypeChecked | TypeChecking);
    return newnode;
}

// Copy the original's type and value into its shell, and re-point every later
// use of the original at the copy
void cloneVarDclFill(CloneState *cstate, VarDclNode *newnode, VarDclNode *node) {
    newnode->vtype = cloneNode(cstate, node->vtype);
    newnode->value = cloneNode(cstate, node->value);
    cloneDclSetMap((INode*)node, (INode*)newnode);
}

// Create a new variable dcl node that is a copy of an existing one
INode *cloneVarDclNode(CloneState *cstate, VarDclNode *node) {
    VarDclNode *newnode = cloneVarDclShell(node);
    cloneVarDclFill(cstate, newnode, node);
    return (INode*)newnode;
}

// Serialize a variable node
void varDclPrint(VarDclNode *name) {
    if (name->flags & FlagStatic)
        inodeFprint("static ");
    inodePrintNode((INode*)name->perm);
    inodeFprint(" %s", &name->namesym->namestr);
    dclInfoPrint((INode*)name);
    inodeFprint(" ");
    inodePrintNode(name->vtype);
    if (name->value) {
        inodeFprint(" = ");
        if (name->value->tag == BlockTag)
            inodePrintNL();
        inodePrintNode(name->value);
    }
}

// Enable name resolution of local variables
void varDclNameRes(NameResState *pstate, VarDclNode *name) {
    inodeNameRes(pstate, (INode**)&name->perm);
    if (name->vtype)
        inodeNameRes(pstate, &name->vtype);

    // Name resolve value before hooking the variable name (so it cannot point to itself)
    if (name->value)
        inodeNameRes(pstate, &name->value);

    // Variable declaration within a block is a local variable
    if (pstate->scope > 0) {
        if (name->namesym->node && pstate->scope == ((VarDclNode*)name->namesym->node)->scope) {
            errorMsgNode((INode *)name, ErrorDupName, "Name is already defined. Only one allowed.");
            errorMsgNode((INode*)name->namesym->node, ErrorDupName, "This is the conflicting definition for that name.");
        }
        else {
            name->scope = pstate->scope;
            // Add name to global name table (containing block will unhook it later)
            nametblHookNode(name->namesym, (INode*)name);
        }
    }
}

// Type check variable against its initial value
void varDclTypeCheck(TypeCheckState *pstate, VarDclNode *name) {
    itypeTypeCheck(pstate, (INode**)&name->perm);
    if (itypeTypeCheck(pstate, &name->vtype) == 0)
        return;

    // A function's static is owned by the function, which is what its symbol is
    // spelled after and what keeps two functions' statics of one name apart. An
    // inline function's body is copied into every caller, so a static in it
    // would be one copy per call site rather than one for all: refused.
    if ((name->flags & FlagStatic) && name->scope > 0 && pstate->fn) {
        if (pstate->fn->flags & FlagInline)
            errorMsgNode((INode*)name, ErrorBadStatic,
                "A static in an inline function would be one copy per call site, not one shared copy.");
        if (name->dclinfo.owner == NULL)
            dclInfoJoin((INode*)name, (INode*)pstate->fn);
    }

    // A parameter holding a borrowed reference holds one of the caller's, whose
    // lifetime is the caller band (1). Its declared type is shared, so it takes
    // a copy scoped so: what it points at is then the caller's however it is
    // reached, and a return or a store reads that from the type. A borrow of the
    // parameter itself is the function's (iexpGetLvalInfo).
    if (name->scope == 1) {
        INode *vtypedcl = itypeGetTypeDcl(name->vtype);
        if (iexpIsBorrowType(vtypedcl))
            name->vtype = iexpScopedBorrowType(vtypedcl, name->vtype, 1);
    }

    // An initializer need not be specified, but if not, it must have a declared type
    if (name->value == NULL) {
        if (name->vtype == unknownType) {
            errorMsgNode((INode*)name, ErrorNoType, "Declared name must specify a type or value");
            return;
        }
    }
    // Type check the initialization value
    else {
        // Verify that declared type and initial value type match
        if (!iexpTypeCheckCoerce(pstate, name->vtype, &name->value))
            errorMsgNode(name->value, ErrorInvType, "Initialization value's type does not match variable's declared type");
        else if (name->vtype == unknownType)
            name->vtype = ((IExpNode *)name->value)->vtype;
        // A declared borrowed-reference type is a shared node carrying no
        // lifetime, so the variable takes a copy of it scoped as its initializer
        // is, as an undeclared one takes the initializer's own scoped type:
        // returning or storing it is then judged by what it was borrowed from
        else if (isExpNode(name->value)) {
            INode *vtypedcl = itypeGetTypeDcl(name->vtype);
            INode *scoped = iexpCoerceType(name->value, vtypedcl);
            if (scoped != vtypedcl)
                name->vtype = scoped;
        }
        // Global variables, function parameters and statics require literal
        // initializers: the value is the storage's initializer, written once
        // before anything runs. A parameter may default to 'srcFile()' or
        // 'srcLine()' too, each a constant where each call taking it is
        // (fnCallFinalizeArgs). An expression of constants is folded into the
        // one it computes (litFoldConst).
        if ((name->scope <= 1 || (name->flags & FlagStatic)) && !litFoldConst(&name->value)
            && !(name->scope == 1 && !(name->flags & FlagStatic) && intrinsicIsSrcCall(name->value)))
            errorMsgNode((INode*)name, ErrorNotLit, "Variable may only be initialized with a literal value.");
    }

    // A global or a static is found by no collector, so it may not hold a
    // traced reference (judged once every type is laid out)
    if (name->scope == 0 || (name->flags & FlagStatic))
        regionTracedGlobalNote(name);

    // A variable holds its type by value, so that type has to be able to say how
    // large it is
    INode *nosizeroot;
    char *nosize = itypeNoSizeCause(name->vtype, &nosizeroot);
    if (nosize) {
        errorMsgNode((INode*)name, ErrorNoSize, "Variable %s cannot be held by value: %s %s.",
            &name->namesym->namestr, itypeName(nosizeroot), nosize);
        itypeNoSizeExplain(name->vtype);
    }
}

// Perform data flow analysis
void varDclFlow(FlowState *fstate, VarDclNode **vardclnode) {
    // A static is not the block's to release: its storage outlives every call,
    // and its literal initializer moves nothing. It holds a value from the
    // start, as a global does, which the parser already recorded.
    if ((*vardclnode)->flags & FlagStatic)
        return;
    flowAddVar(*vardclnode);
    // The temporary an operator changing its operand in place borrows it
    // through ('x += 1', 'v <- (a, b)') is the operator's own, as a method's
    // receiver is, and holds nothing past it
    if ((*vardclnode)->namesym != tempName)
        flowGateHolder(fstate, (*vardclnode)->vtype);
    if ((*vardclnode)->value) {
        flowLoadValue(fstate, &((*vardclnode)->value));
        flowHandleMoveOrCopy(&((*vardclnode)->value));  // initialization copies/moves value
        (*vardclnode)->flowtempflags |= VarInitialized;
    }
}
