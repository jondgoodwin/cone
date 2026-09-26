/** The INode interface underlying all IR nodes.
*
* All IR nodes begin with header fields that specify
* - Which specific node it is, and what node groups it belongs to
* - Node-specific flags distinguishing variations
* - Lexer position info, to improve the helpfulness of error messages
*
* All nodes can be channeled through helpful functions:
* - Dispatch for the semantic passes
* - Serializing the IR nodes (for diagnostic purposes)
*
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef inode_h
#define inode_h

#include "memory.h"

// All IR nodes begin with this header, mostly containing lexer data describing
// where in the source this structure came from (useful for error messages)
// - tag contains the NodeTags code
// - flags contains node-specific flags
// - instnode points to what triggered instancing, if not NULL
// - lexer contains -> url (filepath) and -> source
// - srcp points to start of source token
// - linep points to start of line that token begins on
// - linenbr is the source file's line number, starting with 1
#define INodeHdr \
    INode *instnode; \
    Lexer *lexer; \
    char *srcp; \
    char *linep; \
    uint32_t linenbr; \
    uint16_t tag; \
    uint16_t flags

// INode is a castable struct for all IR nodes.
typedef struct INode {
    INodeHdr;
} INode;

// Typed Node header, offering access to the node's type info
// - vtype is the value type for an expression (e.g., 'i32')
// Note: This header is here because we also need it for type nodes,
// as parser does not know if some nodes are types or value expressions
#define IExpNodeHdr \
    INodeHdr; \
    INode *vtype

// The four groups a node can belong to
typedef enum NodeGroup {
    StmtGroup,      // Statement nodes that return no value
    ExpGroup,       // Nodes that return a typed value
    TypeGroup,      // Nodes that define or refer to a type
    MetaGroup       // Generic, macro, metaconditional nodes
} NodeGroup;

// Easy checks on the kind of node it is. A tag names a node kind and nothing
// more: which group it belongs to, whether it declares a named item, and whether
// it is a type that carries methods are looked up in the tag table in inode.c.
// For every node but a name use the tag is the node's characteristic, so that
// table is the answer. A name use stands for whatever it names, so it answers
// for its declaration (nameUseGroup) and belongs to no group until it is bound
// to one; an unlowered instantiation of a generic type counts as a type
// (itypeIsGenericType). See inodeIsExp and its siblings in inode.c.
#define isExpNode(node) inodeIsExp((INode*)(node))
#define isTypeNode(node) inodeIsType((INode*)(node))
#define isMetaNode(node) inodeIsMeta((INode*)(node))
#define isNamedNode(node) inodeIsNamed((INode*)(node))
#define isMethodType(node) inodeIsMethodType((INode*)(node))
#define isNameUseNode(node) ((node)->tag == NameUseTag)

// A parameterless macro's name stands for the value its body expands to, but it
// is a meta node until type check performs that expansion. A position that
// decides by node kind whether a statement can give a value -- a block's final
// statement, a function's implicit return -- has to count the name as one, or
// it is rejected before it ever gets the chance to expand.
#define isExpOrMacroNode(node) (isExpNode(node) || nameUseNames((INode*)(node), MacroDclTag))

// All the possible tags for a node. The numbers are arbitrary and mean nothing:
// what a tag is -- its group, whether it declares a name, whether it carries
// methods -- is the tag table in inode.c, which needs a row for every tag here.
enum NodeTags {
    ProgramTag,

    // Lexer-only nodes that are *never* found in a program's IR.
    // KeywordTag exists for name table consistency
    KeywordTag,     // Keyword token (flags is the keyword's token type)

    // Untyped (Basic) nodes
    IntrinsicTag,   // Alternative to fndcl block for internal operations (e.g., add)
    ReturnTag,      // Return node
    BlockRetTag,    // Block "return" node. Injected by flow pass for de-aliasing.
    BreakTag,       // Break node
    ContinueTag,    // Continue node
    SwapTag,        // Swap operator
    ImportTag,      // import command
    ModUseTag,      // A module's standalone 'use': an enum's variants or a submodule's names folded in as module names

    // Parser-ambiguous nodes. A name use is an expression, a type or a meta
    // node according to the declaration name resolution binds it to, and is
    // asked rather than retagged (nameUseGroup); the other two are retagged
    // during name resolution when we finally know for sure
    NameUseTag,     // Name use node: stands for whatever its dclnode declares
    TupleTag,       // Tuple for tuple type or tuple literal
    StarTag,        // Could become pointer type or deref exp node

    // Named, non-type declaration nodes
    ModuleTag,      // Module namespace
    ModTraitTag,    // Module trait: the functions and globals a module conforming to it declares or takes
    FnDclTag,       // Function/method declaration
    FnOverloadDclTag, // Overloaded function/method declarations sharing one name
    VarDclTag,      // Variable declaration (global, local, parm)
    FieldDclTag,    // Field declaration in a struct, etc.
    ConstDclTag,    // Constant declaration
    AliasDclTag,    // A binding that stands for another declaration under a spelling of its own

    // Expression nodes (having value type - or sometimes nullType)
    NilLitTag,      // 'nil' literal (of void type)
    ULitTag,        // Integer literal
    FLitTag,        // Float literal
    StringLitTag,   // String literal
    ArrayLitTag,    // Array literal
    TypeLitTag,     // Type literal
    VTupleTag,      // Value tuple (comma-separated values)
    AssignTag,      // Assignment expression
    FnCallTag,      // Function+method call or field access
    ArrIndexTag,    // Array index (FnCallNode)
    FldAccessTag,   // struct field (FnCallNode)
    SizeofTag,      // Sizeof a type (usize)
    CastTag,        // Cast exp to another type
    BorrowTag,      // & (address of) operator
    ArrayBorrowTag, // &[] borrow operator
    AllocateTag,    // & allocated ref allocation
    ArrayAllocTag,  // &[] allocate operator
    DerefTag,       // * (pointed at) operator
    NotLogicTag,    // ! / not
    OrLogicTag,     // or
    AndLogicTag,    // and
    IsTag,          // ~~
    BlockTag,       // Block (list of statements)
    IfTag,          // if .. elif .. else statement
    RefCountTag,    // (injected) adds holders to a counted reference's count
    HollowTag,      // (injected) releases an owning reference whose referent, or an element of it, was moved out
    NamedValTag,    // Named value (e.g., for a struct literal)
    AbsenceTag,     // unique, unclonable node for absence of info

    // Unnamed type node
    FnSigTag,       // Also method, closure, behavior, co-routine, thread, ...
    ArrayTag,       // Also dynamic arrays? SOA?
    RefTag,         // Reference (could become borrowtag/alloctag)
    ArrayRefTag,    // Array reference (slice ref) (could become arrborrow/arralloc tag)
    VirtRefTag,     // Virtual reference
    ArrayDerefTag,  // De-referenced array reference (the slice itself)
    PtrTag,         // Pointer
    TTupleTag,      // Type tuple
    VoidTag,        // a type for "no value", such as no return values for a fn
    QuesTag,        // For "?": may be Option[T] or for allocnode
    BorrowRegTag,   // Borrowed region
    UnknownTag,     // unknown type - must be resolved before gen

    // Named type nodes
    EnumTag,        // Enumerated value
    LifetimeTag,

    // Named type nodes that support methods
    IntNbrTag,      // Integer
    UintNbrTag,     // Unsigned integer
    FloatNbrTag,    // Floating point number
    StructTag,      // struct or trait
    PermTag,

    // Meta group declarations
    MacroDclTag,    // Macro declaration
    GenVarDclTag,   // Generic variable declaration

    NodeTagCount    // How many tags there are: the size of inode.c's tag table
};

// *****************
// Node-specific flags
// *****************

// VarDclTag, FnDclTag and MacroDclTag flags
#define FlagMethFld   0x0001        // FnDcl, VarDcl, MacroDcl: Method or field (vs. static)
// 'extern': defined elsewhere, so no body or value here. The parser's word,
// read once by dclInfoJoin into DclExternal; the name is the module's to spell.
#define FlagExtern    0x0002        // FnDcl, VarDcl: 'extern', defined in another compiled unit
#define FlagInline    0x0008        // FnDcl: "inline" fn/method
// A declaration written 'pub' is visible from outside the namespace that owns
// it; unmarked, it is private to that namespace. Set by the parser on whatever
// declaration the keyword precedes -- fn, variable, field, type, typedef, const
// or macro -- and on an overload name by its candidates (fnOverloadDclAdd).
// 0x0200 is free of every block here: the declaration flags above stop at
// 0x0020 and the type flags skip it.
#define FlagPub       0x0200        // Any declaration: visible outside the namespace that owns it
// One copy shared by every instance of the thing that encloses the variable --
// every call of a function, every value of a type, every instantiation of a
// module -- rather than one copy each. Storage is a global either way; what the
// flag changes is that a local's storage is not an alloca and is not released
// when its block ends. 0x0400 is free as 0x0200 is.
#define FlagStatic    0x0400        // VarDcl: one copy shared across the enclosing thing's instances

#define FlagGenMod    0x0001        // Module: Generate code for the module, if true
#define FlagModDcl    0x0002        // Module: a 'mod' declaration named it, rather than its filename

// An alias whose target is a type expression -- what 'typedef' declares -- as
// against one a fold made, whose target is a member name the fold itself binds.
// It is the one alias with something of its own to name resolve and type check.
// 0x0002 because an alias already reads 0x0001 as FlagMethFld and 0x0200 as
// FlagPub; nothing reads 0x0002 on one.
#define FlagTypeAlias 0x0002        // AliasDcl: target is a type expression, not a folded member name
// The binding an import makes of the imported module's own name, and nothing
// but that: no fold has brought the same module in under the name as well. A
// module's imports are its dependencies, not its contents, so a module that
// extends this one does not take the binding [Jon 23 Sep]. 0x0004 because it is
// a type flag on a type, and an alias is not one; nothing reads 0x0004 on one.
#define FlagImportName 0x0004       // AliasDcl: an import's binding of its module's name, which 'extends' does not carry
// A binding a star clause made -- a wildcard 'use *', a module's 'extends', the
// implicit core import -- and so a name the module never wrote. Where two
// bindings of one declaration meet under one name, they are one binding only if
// one of them is this: a name the module's own source writes twice is an error
// [Jon 23 Sep] (modFoldBind). An enum's 'use' does not set it: 'use Colors;'
// names the enum, so what it binds is written. 0x0008 because it is FlagInline
// on a fn and a type flag on a type, and an alias is neither; nothing reads
// 0x0008 on one.
#define FlagUnlisted  0x0008        // AliasDcl: made by a star clause, not written as this name by the module

#define IsTagField    0x0010        // FieldNode: This field is the trait's discriminant tag
#define IsMixin       0x0020        // FieldNode: a placeholder for a base or a name of an 'is' list, vs. an instantiated field
                                    // (named for the retired 'mixin', whose machinery 'is' builds through)

#define FlagIndex     0x0001        // FnCall: arguments are an index in []
#define FlagBorrow    0x0002        // FnCall: part of a borrow chain
#define FlagVDisp     0x0004        // FnCall: a virtual dispatch function call
#define FlagLvalOp    0x0008        // FnCall: op requires an lval as object (a mutable ref)
#define FlagOpAssgn   0x0010        // FnCall: method is an operator assignment (e.g., +=)
// An operator application ('a * b', '-a', 'a++', 'a += b') and a member access by
// name ('p.sum()') build the same node shape, so only this records which the source
// actually wrote. Set by the three operator constructors in fncall.c, which are the
// only way an operator call is built. 0x0020 is free of every other block: it is
// IsMixin on a FieldDcl and SameSize among the type flags, and a FnCallNode -- also
// when retagged to FldAccess, ArrIndex or TypeLit -- stays in ExpGroup and is
// neither.
#define FlagOperator  0x0020        // FnCall: an operator application, not a named member access
// A macro method's body reaches its receiver's members through 'self', and its
// expansion puts the use site's own expression there. This records that the
// member access was written on 'self', which is what lets the expansion reach a
// private member exactly where the method it stands in for could. Set only by
// cloneFnCallNode, during a macro method's expansion.
#define FlagSelfRecv  0x0040        // FnCall: receiver was a macro method's 'self'
// An index whose argument is a range, 'x[a..b]', which a borrow makes a slice of
// part of an array. Its arguments are the start and, unless the range runs to
// the end ('x[a..]'), the end; FlagRangeIncl marks an end written with '...',
// which includes it. Set only by the parser (parseIndexArgs). 0x0080 and 0x0100
// are free on a FnCall: FirstAssign is a name use's, and the type flags that
// use them are never read off an expression.
#define FlagRange     0x0080        // FnCall: index argument is a range
#define FlagRangeIncl 0x0100        // FnCall: the range's end is included ('...')

#define FlagLoop      0x0001        // Block: is a Loop block
// 'each' lowers to a 'while' whose body ends with the step that advances the loop
// variable, so a 'continue' inside the body would jump over it and the variable
// would never advance. This marks a loop block whose *last* statement is that
// synthesized step, which is what lets blockNameRes copy it ahead of every
// 'continue' that targets the block, and what lets the "break/continue must be
// last" rule ignore a step that displaced the 'continue' the reader wrote last.
// A flag rather than a pointer field, because cloneBlockNode memcpy's the flags
// and clones 'stmts', so a pointer would be left aimed into the original.
#define FlagLoopStep  0x0002        // Block: last statement is 'each's synthesized step

#define FlagSuffix    0x0001        // Borrow: part of a borrow chain

#define FlagQues      0x0001        // Alloc:  Does it return Option[T]?

#define FlagUnkType   0x0001        // ULit: type is unspecified and may be converted to other number
// parsePrefix folds a unary minus into an integer literal by negating its value
// in place, two's complement, so '-1' and '18446744073709551615' hold the same
// bits. This records an odd number of folded minuses, which is what lets
// litCheckRange read the magnitude the source wrote.
#define FlagLitNeg    0x0002        // ULit: value is the negation of the digits written

#define FlagFirstAssign 0x0080      // VarNameUse: assignment target held no prior value
// A name reached through a namespace is written 'math3d.Point3', which parses as
// a member access and is collapsed into a single bound name use by fnCallNameRes.
// This flag is all that survives of the path, and it is all that is wanted: the
// two lowerings that put an implicit 'self' in front of a bare method or field
// name must not fire on a name the source qualified. 0x0001 is free on a name
// use -- the blocks it belongs to are the declaration flags and FnCall's, and a
// NameUseNode is neither.
#define FlagQualified 0x0001        // NameUse: reached through a namespace, not written bare
// A bare name at the root of a pattern -- 'Red' in 'case is Red', 'Circle' in
// 'case imm c &Circle' -- is looked up in the enum of the value being matched
// before it is looked up lexically, and that value's type is known only at type
// check. So the parser marks the name (castPatternMark), name resolution leaves
// it unbound rather than reporting it when nothing lexical answers, and
// castPatternBind binds it at type check and clears the mark. 0x0400 because a
// pattern's root is a type use, and each lower bit is a type flag that some check
// reads off whatever type node it is holding; 0x0400 is read on a VarDcl only.
#define FlagPattern   0x0400        // NameUse: a pattern's bare root name, bound against the matched value first

// Flags used across all types
#define MoveType           0x0001  // Type's values impose move semantics (vs. copy)
#define ThreadBound        0x0002  // Type's value copies must stay in the same thread (vs. sendable)
#define OpaqueType         0x0004  // Type cannot be instantiated as a value (opaque struct, fn, abstract trait)
#define ZeroSizeType       0x0008  // Type has no size in memory (void, empty struct)

// OpaqueType says a value of this type may not be held, and three unrelated
// facts set it: the type was declared @opaque, it is a trait or an '@unsized'
// enum, or a field of it is unsized. Only the first means there is no
// layout to generate -- a trait's own fields are known and are a prefix of every
// implementer's, and a struct with an unsized field is refused by type check
// (ErrorNoSize) long before generation. So generation asks this, not OpaqueType.
#define DeclaredOpaque     0x0100  // Type was declared @opaque: it has no fields

#define TraitType          0x0010  // Is a trait (vs. struct)
#define SameSize           0x0020  // Every variant is padded out to the size of the largest
#define HasTagField        0x0040  // A trait/struct has an enumerated field identifying the variant type
#define NullablePtr        0x0080  // trait/struct has nullable pointer, generating optimized data

// An 'enum': the closed family. Its variants are declared inside it, the
// compiler owns its layout -- the tag at position 0, the common fields spliced
// into every variant, and the padding SameSize asks for -- and its variant set
// is its identity. 'trait' is the open abstraction and carries none of that.
// Read wherever a diagnostic must name the construct the author wrote, and to
// decide what a body may declare. Carried by the enum, not by its variants.
//
// 0x0800 rather than a lower bit: 0x0200 and 0x0400 are spoken for across every
// block, and 0x1000 upward are the progress marks a type carries.
#define EnumType           0x0800  // Is an enum: a closed set of variants

// Type check progress, carried by every declaration. These are type check's
// marks and no other phase's: inodeTypeCheck sets and tests them, and neither
// name resolution nor flow analysis reads or writes them.
//
// TypeChecking means a declaration's type check has begun: it can still answer
// what it has already established, which for a type is its identity but not yet
// its size. TypeChecked means its type check finished -- for a nominal type that
// is *laid out*, set before its methods are checked so that a method may use its
// own type by value. See compiler/c/doc/phases/type-check.md.
#define TypeChecked        0x8000  // Type check of this declaration finished
#define TypeChecking       0x4000  // This declaration's type check has begun

// Name resolution progress, carried by a module and by a struct or trait only:
// the two declarations name resolution reaches by demand rather than in walk
// order. A type's members must be complete -- its own fields and its base's
// defaults in place -- before a type that is-a or mixes it in reads them, so that type
// is resolved when it is first needed, and a module ahead of the modules that
// import it. Set and tested by modNameRes and structNameRes, and read nowhere
// else. 0x1000 and 0x2000 are free of every block above.
#define NameResolved       0x2000  // Name resolution of this module or type finished
#define NameResolving      0x1000  // Name resolution of this module or type has begun

// Allocate and initialize the INode portion of a new node
#define newNode(node, nodestruct, nodetype) {\
    node = (nodestruct*) memAllocBlk(sizeof(nodestruct)); \
    node->tag = nodetype; \
    node->flags = 0; \
    node->instnode = NULL; \
    node->lexer = lex; \
    node->srcp = lex->tokp; \
    node->linep = lex->linep; \
    node->linenbr = lex->linenbr; \
}

// Copy lexer info over to another node
#define copyNodeLex(newnode, oldnode) { \
    (newnode)->lexer = (oldnode)->lexer; \
    (newnode)->srcp = (oldnode)->srcp; \
    (newnode)->linep = (oldnode)->linep; \
    (newnode)->linenbr = (oldnode)->linenbr; \
}

// Copy lexer info over
void inodeLexCopy(INode *new, INode *old);

// Helper functions for serializing a node
void inodePrint(char *dir, char *srcfn, INode *pgm);
void inodePrintNode(INode *node);
void inodeFprint(char *str, ...);
void inodePrintNL();
void inodePrintIndent();
void inodePrintIncr();
void inodePrintDecr();

// Is this node an expression, a type, or a meta node? The isExpNode,
// isTypeNode and isMetaNode macros above are the way to ask.
int inodeIsExp(INode *node);
int inodeIsType(INode *node);
int inodeIsMeta(INode *node);

// Could this node, in a generic's template, be a type once the generic's
// parameters are substituted, though it is not one yet?
int inodeIsProvisionalType(INode *node);

// Does this node declare a named item, and is it a type that supports methods?
// The isNamedNode and isMethodType macros above are the way to ask.
int inodeIsNamed(INode *node);
int inodeIsMethodType(INode *node);

// Obtain name from a named node
Name *inodeGetName(INode *node);

// Obtain the declaration facts of a node that declares a symbol, or NULL if it does not
DclInfo *inodeGetDclInfo(INode *node);

// Obtain the module or type node a declaration lives in, or NULL if it has none
INode *inodeGetOwner(INode *node);

// Is this a declaration that carries its own analysis marks?
int inodeIsDcl(INode *node);

// Determine whether a named node is private: its DclPrivate bit when it carries
// DclInfo, else its spelling
int inodeIsPrivate(INode *node);

// Determine whether a declaration is a member reached through a receiver
int inodeIsMember(INode *node);

// Determine whether an earlier diagnostic already marked this node as bad
int inodeIsError(INode *node);

// Verify IR well-formedness across the whole program (--checktree)
void inodeCheckTree(INode *pgm);

// Dispatch a node walk for the name resolution pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeNameRes(NameResState *pstate, INode **pgm);

// Dispatch a node walk for the current semantic analysis pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
// - expectType is the type expected of an expression node (or unknownType)
// See conec.c for high-level info about the type checking pass
void inodeTypeCheck(TypeCheckState *pstate, INode **pgm, INode *expectType);

// Perform a node walk for the current semantic analysis pass (w/ no type expected)
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeTypeCheckAny(TypeCheckState *pstate, INode **pgm);

#endif
