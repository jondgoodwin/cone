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
extern Name *finalName; // "final": a type's finalizer method, and a module's own finalizer
extern Name *initName;  // "init": a module's initializer

// "tag" -- the discriminant's type. Recognized where a field's type is written
// and nowhere else, so 'pub tag i32' still declares a field named 'tag'. An
// enum places its own discriminant explicitly, for alignment, by writing
// '_ tag' for it.
extern Name *tagName;

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
extern Name *sameName;     // "===", identity: do two references point to the same place?
extern Name *notSameName;  // "!=="
extern Name *leName;       // "<="
extern Name *ltName;       // "<"
extern Name *geName;       // ">="
extern Name *gtName;       // ">"

extern Name *parensName;   // "()"
extern Name *indexName;    // "[]"
extern Name *refIndexName; // "&[]"

extern Name *optionName;   // "Option"

extern Name *rcName;       // "rc"
extern Name *soName;       // "so"
extern Name *allocMethodName;  // "alloc"
extern Name *initMethodName;   // "init"

typedef struct VarDclNode VarDclNode;

// Spell the linker symbol of a declaring node (fn or global variable) into buf,
// which is returned: '_C' and the declaration's path, or the declared name
// alone for a C-style name and for a root declaration that is not an instance
// of a generic. Empty for an unnamed fn.
char *nameSymbol(char *buf, INode *dclnode);

// Spell a type into the buffer, returning the position after it. Where an
// instance of a generic carries its type arguments.
char *nameType(char *bufp, INode *vtype);

// Spell the name of a trait's vtable into buf, which is returned: '<Trait>:Vtable'.
// An LLVM type name, not a symbol.
char *nameVtable(char *buf, INode *trait);

// Spell the symbol of the vtable an implementing type supplies for a trait
// into buf, which is returned: '_CY<type><trait-path>'
char *nameVtableImpl(char *buf, INode *impl, INode *trait);

// Spell the symbol of a trait's vtable list into buf, which is returned: '_CL<trait-path>'
char *nameVtableList(char *buf, INode *trait);

// Spell the symbol of the thunk filling one vtable slot into buf, which is
// returned: '_CY<type><trait-path><slot-ident>'
char *nameVtableThunk(char *buf, INode *impl, INode *trait, Name *slot);


#endif
