/** Standard library initialization
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "../ir/ir.h"
#include "../ir/nametbl.h"
#include "../parser/lexer.h"

#include <string.h>

INode *unknownType;
INode *noCareType;
INode *errorType;
INode *elseCond;
INode *borrowRef;
PermNode *uniPerm;
PermNode *mutPerm;
PermNode *immPerm;
PermNode *roPerm;
PermNode *mut1Perm;
PermNode *opaqPerm;
LifetimeNode *staticLifetimeNode;
NbrNode *boolType;
NbrNode *i8Type;
NbrNode *i16Type;
NbrNode *i32Type;
NbrNode *i64Type;
NbrNode *isizeType;
NbrNode *u8Type;
NbrNode *u16Type;
NbrNode *u32Type;
NbrNode *u64Type;
NbrNode *usizeType;
NbrNode *f32Type;
NbrNode *f64Type;
INsTypeNode *ptrType;
INsTypeNode *refType;
INsTypeNode *arrayRefType;

PermNode *newPermNodeStr(char *name, uint16_t flags) {
    Name *namesym = nametblFind(name, strlen(name));
    PermNode *perm = newPermDclNode(namesym, flags);
    namesym->node = (INode*)perm;
    return perm;
}

// Declare built-in permission types and their names
void stdPermInit() {
    uniPerm = newPermNodeStr("uni", MayRead | MayWrite | RaceSafe | MayIntRefSum | IsLockless);
    mutPerm = newPermNodeStr("mut", MayRead | MayWrite | MayAlias | MayAliasWrite | IsLockless);
    immPerm = newPermNodeStr("imm", MayRead | MayAlias | RaceSafe | MayIntRefSum | IsLockless);
    roPerm = newPermNodeStr("ro", MayRead | MayAlias | IsLockless);
    mut1Perm = newPermNodeStr("mut1", MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless);
    opaqPerm = newPermNodeStr("opaq", MayAlias | RaceSafe | IsLockless);
}

// What core declares in Cone -- Option, Result, and the 'so' and 'rc' regions --
// is the core package's source, packages/core/src/core.cone, not this file's.

FnDclNode *initAllFn;
FnDclNode *finalAllFn;

// A compiler-provided function of no parameters returning nothing, bound as a
// name every module reaches unless it declares the name itself. It has no
// symbol: a call to it is generated as a call to what the intrinsic stands for
static FnDclNode *newStitchFn(char *name, int16_t intrinsic) {
    Name *namesym = nametblFind(name, strlen(name));
    FnSigNode *sig = newFnSigNode();
    sig->rettype = (INode*)newVoidNode();
    FnDclNode *fn = newFnDclNode(namesym, FlagPub, (INode*)sig, (INode*)newIntrinsicNode(intrinsic));
    fn->flags |= TypeChecked;
    namesym->node = (INode*)fn;
    return fn;
}

// Set up the standard library, whose names are always shared by all modules
void stdlibInit(int ptrsize) {

    unknownType = (INode*)newAbsenceNode();
    unknownType->tag = UnknownTag;
    noCareType = (INode*)newAbsenceNode();
    noCareType->tag = UnknownTag;
    // Distinct from unknownType by identity, and deliberately so. unknownType
    // means "not inferred yet", which must not silence anything. errorType means
    // "already reported as bad", which must: every check that would complain
    // about it stays quiet, so one compile reports several real problems instead
    // of a cascade descending from the first.
    errorType = (INode*)newAbsenceNode();
    errorType->tag = UnknownTag;
    elseCond = (INode*)newAbsenceNode();
    borrowRef = (INode*)newAbsenceNode();
    borrowRef->tag = BorrowRegTag;

    staticLifetimeNode = newLifetimeDclNode(nametblFind("'static", 7), 0);
    stdPermInit();
    stdNbrInit(ptrsize);

    // The program's stitched init and final [Jon 23 Sep], callable until the
    // entry glue calls them itself
    initAllFn = newStitchFn("initAll", InitAllIntrinsic);
    finalAllFn = newStitchFn("finalAll", FinalAllIntrinsic);
}
