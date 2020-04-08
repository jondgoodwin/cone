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
    constPerm = newPermNodeStr("const", MayRead | MayAlias | IsLockless);
    mut1Perm = newPermNodeStr("mut1", MayRead | MayWrite | MayAlias | MayIntRefSum | IsLockless);
    opaqPerm = newPermNodeStr("opaq", MayAlias | RaceSafe | IsLockless);
}

AllocNode *newAllocNodeStr(char *name) {
    AllocNode *allocnode;
    newNode(allocnode, AllocNode, RegionTag);
    Name *namesym = nametblFind(name, strlen(name));
    allocnode->namesym = namesym;
    namesym->node = (INode*)allocnode;
    return allocnode;
}

void stdAllocInit() {
    soRegion = newAllocNodeStr("so");
    rcRegion = newAllocNodeStr("rc");
}

// Set up the Option types
void stdOption() {
    // build: enumtrait Option[T] {_ enum}
    optionName = nametblFind("Option", 6);
    Name *TName = nametblFind("T", 1);
    FieldDclNode *tag = newFieldDclNode(anonName, (INode*)immPerm);
    tag->flags |= FlagMethFld;
    tag->vtype = (INode*)newEnumNode();
    StructNode *option = newStructNode(optionName);
    option->flags |= TraitType | SameSize;
    nodelistAdd(&option->fields, (INode*)tag);
    GenVarDclNode *tparm = newGVarDclNode(TName);
    GenericNode *optgen = newGenericDclNode(optionName);
    nodesAdd(&optgen->parms, (INode*)tparm);
    optgen->body = (INode*)option;
    optionName->node = (INode*)optgen;

    // build: struct Some[T] : Option[T] {some T}
    tparm = newGVarDclNode(TName);
    NameUseNode *tnameuse = newNameUseNode(TName);
    tnameuse->tag = GenVarUseTag;
    tnameuse->dclnode = (INode *)tparm;
    FieldDclNode *fld = newFieldDclNode(nametblFind("some",4), (INode*)immPerm);
    fld->flags |= FlagMethFld;
    fld->vtype = (INode*)tnameuse;
    tnameuse = newNameUseNode(TName);
    tnameuse->tag = GenVarUseTag;
    tnameuse->dclnode = (INode *)tparm;
    NameUseNode *basename = newNameUseNode(optionName);
    basename->tag = GenericNameTag;
    basename->dclnode = (INode*)optgen;
    FnCallNode *basegen = newFnCallNode((INode*)basename, 1);
    basegen->flags = FlagIndex;
    nodesAdd(&basegen->args, (INode*)tnameuse);
    Name *someName = nametblFind("Some", 4);
    StructNode *some = newStructNode(someName);
    some->basetrait = (INode*)basegen;
    nodelistAdd(&some->fields, (INode*)fld);
    GenericNode *somegen = newGenericDclNode(optionName);
    nodesAdd(&somegen->parms, (INode*)tparm);
    somegen->body = (INode*)some;
    someName->node = (INode*)somegen;

    // build: struct None[T] : Option[T] {}
    tparm = newGVarDclNode(TName);
    tnameuse = newNameUseNode(TName);
    tnameuse->tag = GenVarUseTag;
    tnameuse->dclnode = (INode *)tparm;
    basename = newNameUseNode(optionName);
    basename->tag = GenericNameTag;
    basename->dclnode = (INode*)optgen;
    basegen = newFnCallNode((INode*)basename, 1);
    basegen->flags = FlagIndex;
    nodesAdd(&basegen->args, (INode*)tnameuse);
    Name *noneName = nametblFind("None", 4);
    StructNode *none = newStructNode(noneName);
    none->basetrait = (INode*)basegen;
    GenericNode *nonegen = newGenericDclNode(optionName);
    nodesAdd(&nonegen->parms, (INode*)tparm);
    nonegen->body = (INode*)none;
    noneName->node = (INode*)nonegen;
}

char *corelib =
"enumtrait Result[T,E] {_ enum;}\n"
"struct Ok[T,E] : Result[T,E] {ok T;}\n"
"struct Error[T,E] : Result[T,E] {error E;}\n"
;

// Set up the standard library, whose names are always shared by all modules
char *stdlibInit(int ptrsize) {
    lexInject("corelib", corelib);

    unknownType = (INode*)newAbsenceNode();
    unknownType->tag = UnknownTag;
    noCareType = (INode*)newAbsenceNode();
    noCareType->tag = UnknownTag;
    elseCond = (INode*)newAbsenceNode();
    borrowRef = (INode*)newAbsenceNode();
    borrowRef->tag = BorrowRegTag;
    noValue = (INode*)newAbsenceNode();

    staticLifetimeNode = newLifetimeDclNode(nametblFind("'static", 7), 0);
    stdPermInit();
    stdAllocInit();
    stdNbrInit(ptrsize);
    stdOption();

    return corelib;
}