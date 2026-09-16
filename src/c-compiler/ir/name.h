/** Name handling - general purpose
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef name_h
#define name_h

#include <stdlib.h>

// Name is an interned symbol, unique by its collection of characters (<=255)
// A name can be hashed into the global name table or a particular node's namespace.
// The struct for a name is an unmovable allocated block in memory
typedef struct Name {
    INode *node;             // Node currently assigned to name
    size_t hash;             // Name's computed hash
    unsigned char namesz;    // Number of characters in the name (<=255)
    char namestr;            // First byte of name's string (the rest follows)
} Name;

// Common symbols - see nametbl.c
extern Name *anonName;  // "_" - the absence of a name
extern Name *tempName;    // "-_"
extern Name *selfName;  // "self"
extern Name *selfTypeName; // "Self"
extern Name *thisName;  // "this"
extern Name *dropName;  // "drop"
extern Name *cloneName; // "clone" method
extern Name *finalName; // "final" method

extern Name *plusEqName;   // "+="
extern Name *minusEqName;  // "-="
extern Name *multEqName;   // "*="
extern Name *divEqName;    // "/="
extern Name *remEqName;    // "%="
extern Name *orEqName;     // "|="
extern Name *andEqName;    // "&="
extern Name *xorEqName;    // "^="
extern Name *shlEqName;    // "<<="
extern Name *shrEqName;    // ">>="
extern Name *lessDashName; // "<-"

extern Name *plusName;     // "+"
extern Name *minusName;    // "-"
extern Name *istrueName;   // "isTrue"
extern Name *multName;     // "*"
extern Name *divName;      // "/"
extern Name *remName;      // "%"
extern Name *orName;       // "|"
extern Name *andName;      // "&"
extern Name *xorName;      // "^"
extern Name *shlName;      // "<<"
extern Name *shrName;      // ">>"

extern Name *incrName;     // "++"
extern Name *decrName;     // "--"
extern Name *incrPostName; // "_++"
extern Name *decrPostName; // "_--"

extern Name *eqName;       // "=="
extern Name *neName;       // "!="
extern Name *leName;       // "<="
extern Name *ltName;       // "<"
extern Name *geName;       // ">="
extern Name *gtName;       // ">"

extern Name *parensName;   // "()"
extern Name *indexName;    // "[]"
extern Name *refIndexName; // "&[]"

extern Name *corelibName;  // "corelib"
extern Name *optionName;   // "Option"

extern Name *rcName;       // "rc"
extern Name *soName;       // "so"
extern Name *allocMethodName;  // "_alloc"
extern Name *initMethodName;   // "init"

typedef struct VarDclNode VarDclNode;
typedef struct FnDclNode FnDclNode;

// Is this function an instance of a generic: instantiated from a generic
// function, or a method of a generic type's instance?
int nameIsGenericInstance(FnDclNode *fn);

// Spell the linker symbol of a declaring node (fn or global variable) into buf,
// which is returned: owner chain, declared name, and a type-argument suffix for
// an instance of a generic. Bare for a C-style name; empty for an unnamed fn.
char *nameSymbol(char *buf, INode *dclnode);

// Spell the name of a trait's vtable into buf, which is returned: '<Trait>:Vtable'
char *nameVtable(char *buf, INode *trait);

// Spell the symbol of the vtable an implementing type supplies for a trait
// into buf, which is returned: '<Impl>-><Trait>:Vtable'
char *nameVtableImpl(char *buf, INode *impl, INode *trait);


#endif
