/** Name handling
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "../shared/memory.h"
#include "../shared/utf8.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

Name *anonName;
Name *tempName;
Name *selfName;
Name *selfTypeName;
Name *thisName;
Name *cloneName;
Name *dropName;
Name *finalName;
Name *initName;
Name *tagName;
Name *plusEqName;
Name *minusEqName;
Name *multEqName;
Name *divEqName;
Name *remEqName;
Name *orEqName;
Name *andEqName;
Name *xorEqName;
Name *shlEqName;
Name *shrEqName;
Name *lessDashName;
Name *plusName;
Name *minusName;
Name *istrueName;
Name *multName;
Name *divName;
Name *remName;
Name *orName;
Name *andName;
Name *xorName;
Name *shlName;
Name *shrName;
Name *incrName;
Name *decrName;
Name *incrPostName;
Name *decrPostName;
Name *eqName;
Name *neName;
Name *sameName;
Name *notSameName;
Name *leName;
Name *ltName;
Name *geName;
Name *gtName;
Name *parensName;
Name *indexName;
Name *refIndexName;
Name *optionName;
Name *allocMethodName;
Name *initMethodName;
Name *aliasMethodName;
Name *dealiasMethodName;
Name *freeMethodName;
Name *regionRefName;

// ---- Identifiers -----------------------------------------------------------
//
// An identifier is spelled <decimal length>[_]<bytes> when its bytes are all in
// the safe set [A-Za-z0-9_], the '_' present when the bytes begin with '_' or a
// digit. Anything else is punycode (RFC 3492), 'u<decimal length>_<bytes>',
// whose basic set is that same safe set rather than all of ASCII and whose
// delimiter is '_'. An operator method's name is 'o' and a two-letter code.

// The operator names a method can be declared with, and their two-letter codes:
// Itanium's where Itanium has the operator, Cone's own where it does not.
static struct NameOperator {
    Name **name;
    char code[3];
} nameOperators[] = {
    {&plusName, "pl"},    {&minusName, "mi"},   {&multName, "ml"},    {&divName, "dv"},
    {&remName, "rm"},     {&eqName, "eq"},      {&neName, "ne"},      {&ltName, "lt"},
    {&leName, "le"},      {&gtName, "gt"},      {&geName, "ge"},      {&andName, "an"},
    {&orName, "or"},      {&xorName, "eo"},     {&shlName, "ls"},     {&shrName, "rs"},
    {&indexName, "ix"},   {&parensName, "cl"},  {&incrName, "pp"},    {&decrName, "mm"},
    {&plusEqName, "pL"},  {&minusEqName, "mI"}, {&multEqName, "mL"},  {&divEqName, "dV"},
    {&remEqName, "rM"},   {&andEqName, "aN"},   {&orEqName, "oR"},    {&xorEqName, "eO"},
    {&shlEqName, "lS"},   {&shrEqName, "rS"},
    // Cone's own: '<-', '&[]', and the postfix forms of '++' and '--'
    {&lessDashName, "la"}, {&refIndexName, "rx"}, {&incrPostName, "pP"}, {&decrPostName, "mM"},
};

static const char *nameOperatorCode(Name *name) {
    for (size_t i = 0; i < sizeof(nameOperators) / sizeof(nameOperators[0]); i++) {
        if (*nameOperators[i].name == name)
            return nameOperators[i].code;
    }
    return NULL;
}

// A code point the encoding passes through unchanged
static int nameIsBasic(uint32_t c) {
    return c < 128 && (isalnum((int)c) || c == '_');
}

// RFC 3492's parameters. A non-basic code point below 128 -- backticked ASCII
// punctuation or a space, which the RFC's basic set would have passed through --
// is lifted above the Unicode range before encoding, so that the initial n of
// 128 stands and an identifier with only non-ASCII characters encodes exactly
// as Rust v0 does.
#define PunyBase 36
#define PunyTmin 1
#define PunyTmax 26
#define PunySkew 38
#define PunyDamp 700
#define PunyInitialBias 72
#define PunyInitialN 128
#define PunyAsciiLift 0x110000

static uint32_t punyAdapt(uint32_t delta, uint32_t numpoints, int firsttime) {
    delta = firsttime ? delta / PunyDamp : delta / 2;
    delta += delta / numpoints;
    uint32_t k = 0;
    while (delta > ((PunyBase - PunyTmin) * PunyTmax) / 2) {
        delta /= PunyBase - PunyTmin;
        k += PunyBase;
    }
    return k + (PunyBase - PunyTmin + 1) * delta / (delta + PunySkew);
}

static char punyDigit(uint32_t d) {
    return (char)(d < 26 ? 'a' + d : '0' + (d - 26));
}

// Punycode-encode the code points into out, returning the position after
static char *punyEncode(char *out, uint32_t *cps, uint32_t len) {
    uint32_t n = PunyInitialN, delta = 0, bias = PunyInitialBias, b = 0, h, i;
    for (i = 0; i < len; i++) {
        if (nameIsBasic(cps[i])) {
            *out++ = (char)cps[i];
            b++;
        }
    }
    h = b;
    if (b > 0)
        *out++ = '_';
    while (h < len) {
        uint32_t m = UINT32_MAX;
        for (i = 0; i < len; i++) {
            if (cps[i] >= n && cps[i] < m)
                m = cps[i];
        }
        delta += (m - n) * (h + 1);
        n = m;
        for (i = 0; i < len; i++) {
            if (cps[i] < n)
                delta++;
            if (cps[i] == n) {
                uint32_t q = delta;
                for (uint32_t k = PunyBase;; k += PunyBase) {
                    uint32_t t = k <= bias ? PunyTmin : k >= bias + PunyTmax ? PunyTmax : k - bias;
                    if (q < t)
                        break;
                    *out++ = punyDigit(t + (q - t) % (PunyBase - t));
                    q = (q - t) / (PunyBase - t);
                }
                *out++ = punyDigit(q);
                bias = punyAdapt(delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++;
        n++;
    }
    return out;
}

// Spell an identifier and return the position after it
static char *nameIdent(char *bufp, Name *name) {
    const char *code = nameOperatorCode(name);
    if (code) {
        *bufp++ = 'o';
        *bufp++ = code[0];
        *bufp++ = code[1];
        return bufp;
    }

    const char *str = &name->namestr;
    size_t len = name->namesz;
    int basic = 1;
    for (size_t i = 0; i < len; i++) {
        if (!nameIsBasic((unsigned char)str[i])) {
            basic = 0;
            break;
        }
    }
    if (basic) {
        bufp += sprintf(bufp, "%u", (unsigned)len);
        if (len > 0 && (str[0] == '_' || isdigit((unsigned char)str[0])))
            *bufp++ = '_';
        memcpy(bufp, str, len);
        return bufp + len;
    }

    // A name is at most 255 bytes, so at most 255 code points
    uint32_t cps[256];
    uint32_t cnt = 0;
    for (const char *p = str; *p; p += utf8ByteSkip(p)) {
        uint32_t c = utf8GetCode(p);
        if (c < 128 && !nameIsBasic(c))
            c += PunyAsciiLift;
        cps[cnt++] = c;
    }
    char puny[2048];
    char *punyend = punyEncode(puny, cps, cnt);
    bufp += sprintf(bufp, "u%u_", (unsigned)(punyend - puny));
    memcpy(bufp, puny, punyend - puny);
    return bufp + (punyend - puny);
}

// ---- Paths and types -------------------------------------------------------

// Does no owner in this chain contribute a path component? True of the root
// module alone and of nothing else, since every other owner is named.
static int nameChainIsEmpty(INode *owner) {
    for (; owner != NULL; owner = inodeGetOwner(owner)) {
        if (owner->tag != ModuleTag || (((ModuleNode*)owner)->dclinfo.facts & DclNamesChain))
            return 0;
    }
    return 1;
}

// The type arguments this declaration is itself an instance of, or NULL. A
// method (or variant) of a generic type's instance carries the instantiating
// node the type's own cloning stamped on it, the same node its owner carries:
// those arguments belong to the owner's path, not to the method's.
static Nodes *nameOwnTypeArgs(INode *node) {
    Nodes *typeargs = itypeInstanceTypeArgs(node);
    if (typeargs == NULL)
        return NULL;
    INode *owner = inodeGetOwner(node);
    if (owner != NULL && owner->instnode == node->instnode)
        return NULL;
    return typeargs;
}

// The declaration a type expression names, through any name use or type alias
static INode *nameTypeDcl(INode *type) {
    while (1) {
        if (isNameUseNode(type) && isTypeNode(type))
            type = nameUseGetDcl((NameUseNode *)type);
        else if (type->tag == AliasDclTag)
            type = ((AliasDclNode *)type)->target;
        else
            return type;
    }
}

// Spell the path of a declaration or named type: 'C<ident>' for a top module,
// 'Nv<parent><ident>' for a fn or global, 'Nt<parent><ident>' for a type or a
// nested module, and 'I<path>{type}E' around an instance of a generic. The root
// module contributes nothing, so a component whose parent is the root has an
// empty parent path.
static char *namePath(char *bufp, INode *node) {
    INode *owner = inodeGetOwner(node);
    Nodes *typeargs = nameOwnTypeArgs(node);
    if (typeargs)
        *bufp++ = 'I';

    if (node->tag == ModuleTag) {
        if (!(((ModuleNode*)node)->dclinfo.facts & DclNamesChain))
            return bufp;
        if (nameChainIsEmpty(owner))
            *bufp++ = 'C';
        else {
            *bufp++ = 'N';
            *bufp++ = 't';
            bufp = namePath(bufp, owner);
        }
    }
    else {
        *bufp++ = 'N';
        *bufp++ = isTypeNode(node) ? 't' : 'v';
        if (owner)
            bufp = namePath(bufp, owner);
    }
    bufp = nameIdent(bufp, inodeGetName(node));

    if (typeargs) {
        INode **argsp;
        uint32_t cnt;
        for (nodesFor(typeargs, cnt, argsp))
            bufp = nameType(bufp, *argsp);
        *bufp++ = 'E';
    }
    return bufp;
}

// The one-letter spelling of a built-in number type, or 0 for any other type
static char nameNbrLetter(INode *type) {
    if (type == (INode*)i8Type) return 'a';
    if (type == (INode*)i16Type) return 's';
    if (type == (INode*)i32Type) return 'l';
    if (type == (INode*)i64Type) return 'x';
    if (type == (INode*)isizeType) return 'i';
    if (type == (INode*)u8Type) return 'h';
    if (type == (INode*)u16Type) return 't';
    if (type == (INode*)u32Type) return 'm';
    if (type == (INode*)u64Type) return 'y';
    if (type == (INode*)usizeType) return 'j';
    if (type == (INode*)f32Type) return 'f';
    if (type == (INode*)f64Type) return 'd';
    if (type == (INode*)boolType) return 'b';
    return 0;
}

// Spell a reference's region and permission: each the identifier of the
// declaration it names. A borrowed reference names no region, which is the
// empty identifier '0'.
static char *nameRegionPerm(char *bufp, RefNode *reftype) {
    if (reftype->region == NULL || reftype->region->tag == BorrowRegTag)
        *bufp++ = '0';
    else
        bufp = nameIdent(bufp, inodeGetName(nameTypeDcl(reftype->region)));
    return nameIdent(bufp, inodeGetName(nameTypeDcl(reftype->perm)));
}

// Spell a type and return the position after it. The distinctions drawn are
// the ones itypeIsSame draws: a reference's region, permission and target,
// but not its lifetime; an array's dimensions; a signature's parameter and
// return types.
char *nameType(char *bufp, INode *vtype) {
    if (isNameUseNode(vtype) && isTypeNode(vtype))
        return nameType(bufp, nameTypeDcl(vtype));
    switch (vtype->tag) {
    case AliasDclTag:
        return nameType(bufp, nameTypeDcl(vtype));

    case UintNbrTag:
    case IntNbrTag:
    case FloatNbrTag:
    {
        char letter = nameNbrLetter(vtype);
        if (letter) {
            *bufp++ = letter;
            return bufp;
        }
        return namePath(bufp, vtype);
    }
    case StructTag:
    case EnumTag:
        return namePath(bufp, vtype);

    case VoidTag:
        *bufp++ = 'u';
        return bufp;

    case RefTag:
    case ArrayRefTag:
    {
        RefNode *reftype = (RefNode *)vtype;
        *bufp++ = vtype->tag == ArrayRefTag ? 'S' : 'R';
        bufp = nameRegionPerm(bufp, reftype);
        return nameType(bufp, reftype->vtexp);
    }
    case VirtRefTag:
    {
        RefNode *reftype = (RefNode *)vtype;
        *bufp++ = 'V';
        bufp = nameRegionPerm(bufp, reftype);
        return namePath(bufp, nameTypeDcl(reftype->vtexp));
    }
    case PtrTag:
        *bufp++ = 'P';
        return nameType(bufp, ((StarNode *)vtype)->vtexp);

    case TTupleTag:
    {
        TupleNode *tuple = (TupleNode *)vtype;
        INode **nodesp;
        uint32_t cnt;
        *bufp++ = 'T';
        for (nodesFor(tuple->elems, cnt, nodesp))
            bufp = nameType(bufp, *nodesp);
        *bufp++ = 'E';
        return bufp;
    }
    case ArrayTag:
    {
        // An array of several dimensions is an array of arrays, outermost
        // dimension last: 'A<type><decimal>_' per dimension. Every dimension
        // is a ULitNode by the time a type is spelled; one that is not has
        // already been reported.
        ArrayNode *anode = (ArrayNode *)vtype;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(anode->dimens, cnt, nodesp))
            *bufp++ = 'A';
        bufp = nameType(bufp, arrayElemType(vtype));
        uint32_t dims = anode->dimens->used;
        while (dims-- > 0) {
            INode *dim = nodesGet(anode->dimens, dims);
            unsigned long long size = dim->tag == ULitTag ? ((ULitNode*)dim)->uintlit : 0;
            bufp += sprintf(bufp, "%llu_", size);
        }
        return bufp;
    }
    case FnSigTag:
    {
        FnSigNode *fnsig = (FnSigNode *)vtype;
        INode **nodesp;
        uint32_t cnt;
        *bufp++ = 'F';
        for (nodesFor(fnsig->parms, cnt, nodesp))
            bufp = nameType(bufp, ((IExpNode *)*nodesp)->vtype);
        *bufp++ = 'E';
        return nameType(bufp, fnsig->rettype);
    }
    default:
        errorUnreachable(vtype, "a type the symbol speller has no case for");
        return bufp;
    }
}

// ---- Symbols ---------------------------------------------------------------

// Spell the linker symbol of a declaring node (fn or global variable) into buf,
// which is returned. A C name is what '@c' states: the fn's own string, or the
// C-named module's prefix and the declared name. A declaration with nothing to
// encode -- an empty owner chain and not an instance of a generic, which is
// every root fn and global including 'main' -- is its declared name alone,
// 'extern' or not. Everything else, an 'extern' declaration included, is
// '_C' and its path. A function with no name at all (a lifted 'fn' literal)
// spells the empty string, and is named at generation instead.
char *nameSymbol(char *buf, INode *dclnode) {
    *buf = '\0';
    Name *name = inodeGetName(dclnode);
    if (name == NULL)
        return buf;

    DclInfo *dclinfo = inodeGetDclInfo(dclnode);
    // A C name is stated, never derived [Jon 27 Aug]: the declaration's own
    // '@c("sym")' is the whole symbol; otherwise it is its name, after the
    // prefix its C-named module's '@c("prefix")' states, if it has one
    if (dclinfo->facts & DclCName) {
        if (dclinfo->cname) {
            strcpy(buf, dclinfo->cname);
            return buf;
        }
        INode *owner = dclinfo->owner;
        char *prefix = owner && owner->tag == ModuleTag
            && (((ModuleNode*)owner)->dclinfo.facts & DclCName) ? ((ModuleNode*)owner)->dclinfo.cname : NULL;
        if (prefix) {
            strcpy(buf, prefix);
            strcat(buf, &name->namestr);
        }
        else
            strcpy(buf, &name->namestr);
        return buf;
    }
    if (nameChainIsEmpty(dclinfo->owner) && nameOwnTypeArgs(dclnode) == NULL) {
        strcpy(buf, &name->namestr);
        return buf;
    }
    char *bufp = buf;
    *bufp++ = '_';
    *bufp++ = 'C';
    bufp = namePath(bufp, dclnode);
    *bufp = '\0';
    return buf;
}

// Spell the name of a trait's vtable into buf, which is returned: '<Trait>:Vtable'.
// It names both the vtable's LLVM type and the virtual reference's -- LLVM type
// names, not object-file symbols, so they are not encoded.
char *nameVtable(char *buf, INode *trait) {
    char *bufp = buf;
    strcpy(bufp, &inodeGetName(trait)->namestr);
    bufp += strlen(bufp);
    strcpy(bufp, ":Vtable");
    return buf;
}

// Spell the symbol of the vtable an implementing type supplies for a trait
// into buf, which is returned: '_CY<type><trait-path>', this type as that trait
char *nameVtableImpl(char *buf, INode *impl, INode *trait) {
    char *bufp = buf;
    *bufp++ = '_';
    *bufp++ = 'C';
    *bufp++ = 'Y';
    bufp = nameType(bufp, impl);
    bufp = namePath(bufp, trait);
    *bufp = '\0';
    return buf;
}

// Spell the symbol of the thunk that fills one slot of an implementing type's
// vtable for a trait into buf, which is returned: '_CY<type><trait-path><ident>',
// this type as that trait, then the slot's name. Only a slot a folded method
// fills has one: the thunk shifts the receiver to the field the method was
// folded through and tail-calls the method, and nothing in the language can
// name it.
char *nameVtableThunk(char *buf, INode *impl, INode *trait, Name *slot) {
    char *bufp = buf;
    *bufp++ = '_';
    *bufp++ = 'C';
    *bufp++ = 'Y';
    bufp = nameType(bufp, impl);
    bufp = namePath(bufp, trait);
    bufp = nameIdent(bufp, slot);
    *bufp = '\0';
    return buf;
}

// Spell the symbol of a trait's list of vtables into buf, which is returned:
// '_CL<trait-path>'. One per trait per compilation unit.
char *nameVtableList(char *buf, INode *trait) {
    char *bufp = buf;
    *bufp++ = '_';
    *bufp++ = 'C';
    *bufp++ = 'L';
    bufp = namePath(bufp, trait);
    *bufp = '\0';
    return buf;
}
