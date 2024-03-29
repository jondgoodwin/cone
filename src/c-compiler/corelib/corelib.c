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

char *corelibSource =
"union Option[T] {\n"
  "struct None {}\n"
  "struct Some {value T}\n"
"}\n"

"union Result[T,E] {\n"
  "struct Ok {value T}\n"
  "struct Error {value E}\n"
"}\n"

"extern fn malloc(size usize) *u8\n"

"struct @move so:\n"
"  fn _alloc(size usize) *u8 inline {malloc(size)}\n"

"struct rc:\n"
"  cnt usize\n"
"  fn _alloc(size usize) *u8 inline {malloc(size)}\n"
"  fn init() rc inline {rc[1usize]}\n"
;

// Set up the standard library, whose names are always shared by all modules
void stdlibInit(int ptrsize) {

    unknownType = (INode*)newAbsenceNode();
    unknownType->tag = UnknownTag;
    noCareType = (INode*)newAbsenceNode();
    noCareType->tag = UnknownTag;
    elseCond = (INode*)newAbsenceNode();
    borrowRef = (INode*)newAbsenceNode();
    borrowRef->tag = BorrowRegTag;

    staticLifetimeNode = newLifetimeDclNode(nametblFind("'static", 7), 0);
    stdPermInit();
    stdNbrInit(ptrsize);
}
