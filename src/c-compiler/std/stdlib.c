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

Name *keyAdd(char *keyword, uint16_t toktype) {
    Name *sym;
    INode *node;
    sym = nametblFind(keyword, strlen(keyword));
    sym->node = node = (INode*)memAllocBlk(sizeof(INode));
    node->tag = KeywordTag;
    node->flags = toktype;
    return sym;
}

void keywordInit() {
    keyAdd("include", IncludeToken);
    keyAdd("mod", ModToken);
    keyAdd("extern", ExternToken);
    keyAdd("set", SetToken);
    keyAdd("macro", MacroToken);
    keyAdd("fn", FnToken);
    keyAdd("typedef", TypedefToken),
    keyAdd("struct", StructToken);
    keyAdd("trait", TraitToken);
    keyAdd("mixin", MixinToken);
    keyAdd("enumtrait", EnumTraitToken);
    keyAdd("enum", EnumToken);
    keyAdd("region", RegionToken);
    keyAdd("return", RetToken);
    keyAdd("do", DoToken);
    keyAdd("with", WithToken);
    keyAdd("if", IfToken);
    keyAdd("elif", ElifToken);
    keyAdd("else", ElseToken);
    keyAdd("match", MatchToken);
    keyAdd("loop", LoopToken);
    keyAdd("while", WhileToken);
    keyAdd("each", EachToken);
    keyAdd("in", InToken);
    keyAdd("by", ByToken);
    keyAdd("break", BreakToken);
    keyAdd("continue", ContinueToken);
    keyAdd("not", NotToken);
    keyAdd("or", OrToken);
    keyAdd("and", AndToken);
    keyAdd("as", AsToken);
    keyAdd("is", IsToken);
    keyAdd("into", IntoToken);

    keyAdd("true", trueToken);
    keyAdd("false", falseToken);
    keyAdd("null", nullToken);
}

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
    GenericNode *optgen = newGenericNode(optionName);
    optgen->flags |= GenericMemoize;
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
    basename->tag = MacroNameTag;
    basename->dclnode = (INode*)optgen;
    FnCallNode *basegen = newFnCallNode((INode*)basename, 1);
    basegen->flags = FlagIndex;
    nodesAdd(&basegen->args, (INode*)tnameuse);
    Name *someName = nametblFind("Some", 4);
    StructNode *some = newStructNode(someName);
    some->basetrait = (INode*)basegen;
    nodelistAdd(&some->fields, (INode*)fld);
    GenericNode *somegen = newGenericNode(optionName);
    somegen->flags |= GenericMemoize;
    nodesAdd(&somegen->parms, (INode*)tparm);
    somegen->body = (INode*)some;
    someName->node = (INode*)somegen;

    // build: struct None[T] : Option[T] {}
    tparm = newGVarDclNode(TName);
    tnameuse = newNameUseNode(TName);
    tnameuse->tag = GenVarUseTag;
    tnameuse->dclnode = (INode *)tparm;
    basename = newNameUseNode(optionName);
    basename->tag = MacroNameTag;
    basename->dclnode = (INode*)optgen;
    basegen = newFnCallNode((INode*)basename, 1);
    basegen->flags = FlagIndex;
    nodesAdd(&basegen->args, (INode*)tnameuse);
    Name *noneName = nametblFind("None", 4);
    StructNode *none = newStructNode(noneName);
    none->basetrait = (INode*)basegen;
    GenericNode *nonegen = newGenericNode(optionName);
    nonegen->flags |= GenericMemoize;
    nodesAdd(&nonegen->parms, (INode*)tparm);
    nonegen->body = (INode*)none;
    noneName->node = (INode*)nonegen;
}

// Set up the standard library, whose names are always shared by all modules
void stdlibInit(int ptrsize) {
    lexInject("std", "");

    anonName = nametblFind("_", 1);
    selfName = nametblFind("self", 4);
    selfTypeName = nametblFind("Self", 4);
    thisName = nametblFind("this", 4);
    cloneName = nametblFind("clone", 5);
    finalName = nametblFind("final", 5);

    plusEqName = nametblFind("+=", 2);
    minusEqName = nametblFind("-=", 2);
    multEqName = nametblFind("*=", 2);
    divEqName = nametblFind("/=", 2);
    remEqName = nametblFind("%=", 2);
    orEqName = nametblFind("|=", 2);
    andEqName = nametblFind("&=", 2);
    xorEqName = nametblFind("^=", 2);
    shlEqName = nametblFind("<<=", 3);
    shrEqName = nametblFind(">>=", 3);

    plusName = nametblFind("+", 1);
    minusName = nametblFind("-", 1);
    multName = nametblFind("*", 1);
    divName = nametblFind("/", 1);
    remName = nametblFind("%", 1);
    orName = nametblFind("|", 1);
    andName = nametblFind("&", 1);
    xorName = nametblFind("^", 1);
    shlName = nametblFind("<<", 2);
    shrName = nametblFind(">>", 2);

    incrName = nametblFind("++", 2);
    decrName = nametblFind("--", 2);
    incrPostName = nametblFind("+++", 3);
    decrPostName = nametblFind("---", 3);

    eqName = nametblFind("==", 2);
    neName = nametblFind("!=", 2);
    leName = nametblFind("<=", 2);
    ltName = nametblFind("<", 1);
    geName = nametblFind(">=", 2);
    gtName = nametblFind(">", 1);

    parensName = nametblFind("()", 2);
    indexName = nametblFind("[]", 2);
    refIndexName = nametblFind("&[]", 3);

    unknownType = (INode*)newAbsenceNode();
    unknownType->tag = UnknownTag;
    noCareType = (INode*)newAbsenceNode();
    noCareType->tag = UnknownTag;
    elseCond = (INode*)newAbsenceNode();
    borrowRef = (INode*)newAbsenceNode();
    borrowRef->tag = BorrowRegTag;
    noValue = (INode*)newAbsenceNode();

    keywordInit();
    staticLifetimeNode = newLifetimeDclNode(nametblFind("'static", 7), 0);
    stdPermInit();
    stdAllocInit();
    stdNbrInit(ptrsize);
    stdOption();
}