/** Standard library initialiation
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef stdlib_h
#define stdlib_h

// Unique (unclonable) nodes representing the absence of value or type
extern INode *unknownType;   // Unknown/unspecified type
extern INode *noCareType;    // When the receiver does not care what type is returned
extern INode *errorType;     // The type of a node already reported as bad
extern INode *nullLitType;   // The type of a 'null' not yet given its pointer type
extern INode *elseCond;   // node representing the 'else' condition for an 'if' node
extern INode *borrowRef;  // When a reference's region is unspecified, as it is borrowed

// Built-in permission types - for implicit (non-declared but known) permissions
extern PermNode *uniPerm;
extern PermNode *mutPerm;
extern PermNode *immPerm;
extern PermNode *roPerm;
extern PermNode *mut1Perm;
extern PermNode *opaqPerm;
// 'new': an initializer's 'self', a reference to memory that does not hold its
// value yet. Written '&new', after the keyword, so its name is not a permission
// token.
extern PermNode *newPerm;

// Primitive numeric types - for implicit (nondeclared but known) types
extern NbrNode *boolType;    // i1
extern NbrNode *charType;    // a Unicode code point, i32: a 32-bit unsigned by tag, no arithmetic
extern NbrNode *i8Type;
extern NbrNode *i16Type;
extern NbrNode *i32Type;
extern NbrNode *i64Type;
extern NbrNode *isizeType;
extern NbrNode *u8Type;
extern NbrNode *u16Type;
extern NbrNode *u32Type;
extern NbrNode *u64Type;
extern NbrNode *usizeType;
extern NbrNode *f32Type;
extern NbrNode *f64Type;

extern INsTypeNode *ptrType;
extern INsTypeNode *refType;
extern INsTypeNode *arrayRefType;

// 'Never', the return type of a function that does not return (stdlibInit):
// a void type, told apart from every other by identity (itypeIsNever)
extern INode *neverType;

// 'Array', the name the array type is written with, 'Array[T, n]' (stdlibInit):
// a struct with no fields that only names it, since name resolution lowers
// every bracketed use into an array type node (arrayTypeLower)
extern StructNode *arrayTypeDcl;

// The compiler's own 'Array' struct, when module 'mod' is core declaring 'Array[T]'
// for the first time, so that core's declaration makes it the generic body of a
// run-time length instead of a second struct of the name; else NULL.
StructNode *stdlibAdoptArray(Name *name, ModuleNode *mod);

// Is this declaration the generic body 'Array[T]', which a borrow of makes the slice
// (refNameRes)? It is the compiler's own, once core has declared it; or, while a
// library's include file is checked (stdlibIncludeChecking), the 'Array[T]' of the
// copy of core being checked, which the first copy has already made the compiler's.
int stdlibIsArrayBody(INode *dcl);

// Set while writeIncludeFile checks the text it generated for a library
extern int stdlibIncludeChecking;

// 'str', the dynamically sized body of bytes (stdlibInit): a type with no
// fields and no size of its own, held only through a reference, which carries
// the number of bytes (itypeLenBodyElem, refIsFat)
extern StructNode *strTypeDcl;

// The compiler's own 'str' struct, when module 'mod' is core declaring 'str' for
// the first time, so that core's declaration gives it its methods instead of
// making a second struct of the name; else NULL. Its flags are those of a
// declaration still to be name resolved and type checked.
StructNode *stdlibAdoptStr(Name *name, ModuleNode *mod);

// 'cstr', a borrowed C string (stdlibInit): a struct of one raw pointer to bytes
// that end in a NUL, which C's 'const char *' is. The compiler makes it so that
// the C-named modules, which get no prelude, can declare it in their bindings.
extern StructNode *cstrTypeDcl;

// The same for 'cstr': core's declaration of it gives the compiler's struct its
// field and its methods. The struct is handed back empty.
StructNode *stdlibAdoptCStr(Name *name, ModuleNode *mod);

// The two functions a program calls to run its stitched init and final
// (genlStitch): 'initAll()' and 'finalAll()', names every module reaches as it
// reaches 'i64', and which a declaration of its own hides
extern FnDclNode *initAllFn;
extern FnDclNode *finalAllFn;

// The built-in traits, each a name every module reaches unless it declares the
// name itself, and none with members: 'RegionRef', which a region ref struct
// declares with 'is'; 'Move' and 'Copy', one of which every type has, the
// compiler granting it from what it infers and a type able to declare it; and
// 'Traced', which a region ref struct declares to say its references are traced;
// 'ThreadSafe', which one declares to say several threads may hold its owners;
// and what a container's element borrows cost it: 'ShapeChanging' (it may move
// its elements), 'NoLoanMut' and 'NoLoanRead' (its element borrows loan nothing);
// and 'AtomicValue', a value changed only by atomic operations; and 'Integer'
// and 'Pointer', which the compiler grants the integer types and the raw
// pointer types, for a constraint to ask of
extern StructNode *regionRefTrait;
extern StructNode *moveTrait;
extern StructNode *copyTrait;
extern StructNode *tracedTrait;
extern StructNode *threadSafeTrait;
extern StructNode *shapeChangingTrait;
extern StructNode *noLoanMutTrait;
extern StructNode *noLoanReadTrait;
extern StructNode *atomicValueTrait;
extern StructNode *integerTrait;
extern StructNode *pointerTrait;
extern StructNode *sendableTrait;
// 'Sized' and 'DynSized', which the compiler grants from a type's size: a type
// whose size is known at compile time is Sized, and DynSized too; so is a type
// whose size a reference to it carries, a trait (its vtable) or a body such
// as 'str' (its length). A type with neither, one declared @opaque or an
// '@unsized' enum, is held through a thin reference only
extern StructNode *sizedTrait;
extern StructNode *dynSizedTrait;
// 'Immutable', which a type declares with 'is' ('str' does, in stdlibInit): a
// reference to it written with no permission is 'imm', and 'mut', 'mut1' and a
// lock permission are refused on it (refImmutableDefaultPerm, refImmutableBan)
extern StructNode *immutableTrait;
// 'LockPermission', which a struct declares to stand in a managed reference's
// permission slot: 'Arc[Mutex, T]' (ir/types/permission.c)
extern StructNode *lockPermTrait;

// Is this declaration one of the built-in traits?
int corelibIsBuiltinTrait(INode *node);

void stdlibInit(int ptrsize);
void keywordInit();
void stdNbrInit(int ptrsize);

// Give every integer type and bool the method core's Hash requires of them,
// 'hash(self, h &mut Hasher)', once core's Hasher is name resolved
void nbrAddHashMethods(StructNode *hasher);

#endif
