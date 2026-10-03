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
// "-temp": the hidden local of a block that a temporary its borrow extends
// becomes ('imm r = &make();', varDclExtend). No source can spell it.
extern Name *tempLocalName;
extern Name *selfName;  // "self"
extern Name *staticLifeName;  // "'static", the global lifetime (lifetime.h)
extern Name *unknownBrandName; // "'=?", an invariant lifetime nothing is known of: the same as none
extern Name *selfTypeName; // "Self"
extern Name *thisName;  // "this"
extern Name *dropName;  // "drop": the finalizer the compiler gives a module
// "-drop": the drop the compiler gives a type (structSetDropFn,
// structSetEnumDropFn). 'drop' is an ordinary method name, so the generated one
// takes a name no plainly written method has, and the two never share a symbol.
extern Name *typeDropName;
extern Name *cloneName; // "clone" method
extern Name *finalName; // "final": a type's finalizer method, and a module's own finalizer
// "-final": an enum's 'final' as cloned into a variant that declares its own,
// which the variant's drop calls right after that one. No source can spell it,
// so it collides with no method a variant declares and no name reaches it.
extern Name *enumFinalName;
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

extern Name *fromName;     // "from", a number type's conversion: 'u64.from(count)'

// The contextual words of a '<-' list's entries, 'n of x' and 'fill x': names
// everywhere else, read as words only on the right of '<-' (parseEntry); and
// the two methods 'fill' asks a collection, how many values it holds and how
// many it has room for (contents.c)
extern Name *ofName;       // "of"
extern Name *fillName;     // "fill"
extern Name *lenName;      // "len"
extern Name *capacityName; // "capacity"

// The methods a region's annotation struct may declare, which the compiler
// calls at each reference event (ir/types/region.c), and the built-in trait
// that checks them
extern Name *allocMethodName;  // "alloc"
extern Name *initMethodName;   // "init"
extern Name *aliasRefMethodName;  // "aliasRef"
extern Name *dealiasRefMethodName; // "dealiasRef"
extern Name *freeMethodName;   // "free"
extern Name *markMethodName;   // "mark", which a traced region's trace calls
extern Name *writeBarrierMethodName; // "writeBarrier", called after a traced reference is stored where no root is
extern Name *regionRefName;    // "RegionRef"
extern Name *tracedTraitName;  // "Traced", a region ref whose references are traced
extern Name *threadSafeTraitName; // "ThreadSafe", a region ref whose owners several threads may hold

// The built-in marker traits every type has exactly one of (corelib.c)
extern Name *moveTraitName;    // "Move"
extern Name *copyTraitName;    // "Copy"
extern Name *shapeChangingTraitName; // "ShapeChanging", a container that may move its elements
extern Name *noLoanMutTraitName;     // "NoLoanMut", a container whose mutable element borrows loan nothing
extern Name *noLoanReadTraitName;    // "NoLoanRead", a container whose read-only element borrows loan nothing
extern Name *atomicValueTraitName;   // "AtomicValue", a value changed only by atomic operations
extern Name *integerTraitName;   // "Integer", the integer types, which a constraint may ask for
extern Name *pointerTraitName;   // "Pointer", the raw pointer types, which a constraint may ask for
extern Name *sendableTraitName;  // "Sendable", a type whose values may cross to another thread, which a constraint may ask for

// The built-in trait a lock permission declares, and the methods the compiler
// calls on it as a borrow through a lock-managed reference begins and ends
// (ir/types/permission.c)
extern Name *lockPermTraitName;     // "LockPermission"
extern Name *acquireMutMethodName;  // "acquireMut", taken for a mutable borrow
extern Name *releaseMutMethodName;  // "releaseMut"
extern Name *acquireReadMethodName; // "acquireRead", taken for a read-only borrow, where declared
extern Name *releaseReadMethodName; // "releaseRead"

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
