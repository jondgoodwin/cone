/** Name handling
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "ir.h"
#include "../shared/memory.h"

#include <stdint.h>
#include <string.h>

Name *anonName;
Name *tempName;
Name *selfName;
Name *selfTypeName;
Name *thisName;
Name *cloneName;
Name *dropName;
Name *finalName;
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
Name *leName;
Name *ltName;
Name *geName;
Name *gtName;
Name *parensName;
Name *indexName;
Name *refIndexName;
Name *corelibName;
Name *optionName;
Name *rcName;
Name *soName;
Name *allocMethodName;
Name *initMethodName;

// Is this function an instance of a generic? Either it was itself instantiated
// from a generic function, or it is a method of a generic type's instance. Only
// such a function carries a type-argument suffix and merges across object files.
// A trait default cloned into an ordinary implementing type is neither: it is a
// copy owned by that type, spelled and linked like a method written there.
int nameIsGenericInstance(FnDclNode *fn) {
    if (itypeInstanceTypeArgs((INode*)fn) != NULL)
        return 1;
    INode *owner = inodeGetOwner((INode*)fn);
    return owner != NULL && owner->tag == StructTag && itypeInstanceTypeArgs(owner) != NULL;
}

// Append a name and return the position after it
static char *nameAppend(char *bufp, Name *name) {
    strcpy(bufp, &name->namestr);
    return bufp + strlen(bufp);
}

// Append the owner chain, outermost first, each owner as '<name>_'. A module
// contributes its name only when it says so: the program's root does not, which
// is why a root declaration (and so 'main') is spelled bare. A type contributes
// its declared name alone, without the suffix its own instance would carry.
static char *nameOwnerChain(char *bufp, INode *owner) {
    if (owner == NULL)
        return bufp;
    bufp = nameOwnerChain(bufp, inodeGetOwner(owner));
    if (owner->tag == ModuleTag && !(((ModuleNode*)owner)->dclinfo.facts & DclNamesChain))
        return bufp;
    Name *name = isNamedNode(owner) ? inodeGetName(owner) : NULL;
    if (name == NULL)
        return bufp;
    bufp = nameAppend(bufp, name);
    *bufp++ = '_';
    return bufp;
}

// Spell the linker symbol of a declaring node (fn or global variable) into buf,
// which is returned: the owner chain, the declared name, and for an instance of
// a generic a ':'-separated mangling of each parameter's type, which is where
// the type arguments show. A C-style name is the declared name alone. A function
// with no name at all (a lifted 'fn' literal) spells the empty string, and is
// named at generation instead.
char *nameSymbol(char *buf, INode *dclnode) {
    char *bufp = buf;
    *bufp = '\0';
    Name *name = inodeGetName(dclnode);
    if (name == NULL)
        return buf;

    DclInfo *dclinfo = inodeGetDclInfo(dclnode);
    if (!(dclinfo->facts & DclCName))
        bufp = nameOwnerChain(bufp, dclinfo->owner);
    bufp = nameAppend(bufp, name);

    if (dclnode->tag == FnDclTag && nameIsGenericInstance((FnDclNode*)dclnode)) {
        FnSigNode *fnsig = (FnSigNode *)((FnDclNode*)dclnode)->vtype;
        INode **nodesp;
        uint32_t cnt;
        for (nodesFor(fnsig->parms, cnt, nodesp)) {
            *bufp++ = ':';
            bufp = itypeMangle(bufp, ((IExpNode *)*nodesp)->vtype);
        }
        *bufp = '\0';
    }
    return buf;
}

// Spell the name of a trait's vtable into buf, which is returned: '<Trait>:Vtable'.
// It names both the vtable's LLVM type and the virtual reference's.
char *nameVtable(char *buf, INode *trait) {
    char *bufp = nameAppend(buf, inodeGetName(trait));
    strcpy(bufp, ":Vtable");
    return buf;
}

// Spell the symbol of the vtable an implementing type supplies for a trait into
// buf, which is returned: '<Impl>-><Trait>:Vtable'
char *nameVtableImpl(char *buf, INode *impl, INode *trait) {
    char *bufp = nameAppend(buf, inodeGetName(impl));
    strcpy(bufp, "->");
    nameVtable(bufp + 2, trait);
    return buf;
}
