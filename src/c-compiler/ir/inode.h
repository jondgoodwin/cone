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

// Flags found at the top of a node's tag
#define StmtGroup  0x0000   // Statement nodes that return no value
#define ExpGroup   0x4000   // Nodes that return a typed value
#define TypeGroup  0x8000   // Nodes that define or refer to a type
#define MetaGroup  0xC000   // Generic, macro, metaconditional nodes
#define GroupMask  0xC000
#define NamedNode  0x2000   // Node that defines a named item (not nameuse)
#define MethodType 0x1000   // Type that supports methods

// Easy checks on the kind of node it is based on high-level flags
#define isExpNode(node) (((node)->tag & GroupMask) == ExpGroup)
#define isTypeNode(node) (((node)->tag & GroupMask) == TypeGroup || itypeIsGenericType(node))
#define isMetaNode(node) (((node)->tag & GroupMask) == MetaGroup)
#define isNamedNode(node) ((node)->tag & NamedNode)
#define isMethodType(node) (isTypeNode(node) && ((node)->tag & MethodType))

// A parameterless macro's name stands for the value its body expands to, but it
// is a meta node until type check performs that expansion. A position that
// decides by node kind whether a statement can give a value -- a block's final
// statement, a function's implicit return -- has to count the name as one, or
// it is rejected before it ever gets the chance to expand.
#define isExpOrMacroNode(node) (isExpNode(node) || (node)->tag == MacroNameTag)

// All the possible tags for a node
enum NodeTags {
    ProgramTag = StmtGroup,

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

    // Parser-ambiguous nodes that will become either types or expressions
    // during name resolution when we finally know for sure
    NameUseTag,     // Name use node (pre-name resolution)
    TupleTag,       // Tuple for tuple type or tuple literal
    StarTag,        // Could become pointer type or deref exp node

    // Named, non-type declaration nodes
    ModuleTag = StmtGroup + NamedNode,        // Module namespace
    FnDclTag,       // Function/method declaration
    FnOverloadDclTag, // Overloaded function/method declarations sharing one name
    VarDclTag,      // Variable declaration (global, local, parm)
    FieldDclTag,    // Field declaration in a struct, etc.
    ConstDclTag,    // Constant declaration

    // Expression nodes (having value type - or sometimes nullType)
    VarNameUseTag = ExpGroup,  // Variable or Function name use node  
    MbrNameUseTag,  // Member of a type's namespace (field/method)
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
    AliasTag,       // (injected) alias count tag
    NamedValTag,    // Named value (e.g., for a struct literal)
    AbsenceTag,     // unique, unclonable node for absence of info

    // Unnamed type node
    TypeNameUseTag = TypeGroup, // Type name use node
    TypedefTag,     // A type name alias (structural)
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

    EnumTag = TypeGroup + NamedNode,    // Enumerated value
    LifetimeTag,

    IntNbrTag = TypeGroup + NamedNode + MethodType,    // Integer
    UintNbrTag,     // Unsigned integer
    FloatNbrTag,    // Floating point number
    StructTag,      // struct or trait
    PermTag,

    // Meta group names
    MacroNameTag = MetaGroup,   // Macro name use node
    GenericNameTag,             // Generic name use node
    GenVarUseTag,               // Generic variable name use

    MacroDclTag = MetaGroup + NamedNode,     // Macro declaration
    GenVarDclTag,               // Generic variable declaration

};

// *****************
// Node-specific flags
// *****************

// VarDclTag and FnDclTag flags
#define FlagMethFld   0x0001        // FnDcl, VarDcl: Method or field (vs. static)
#define FlagExtern    0x0002        // FnDcl, VarDcl: C ABI extern (no value, no mangle)
#define FlagSystem    0x0004        // FnDcl: imported system call (+stdcall on Winx86)
#define FlagInline    0x0008        // FnDcl: "inline" fn/method

#define FlagGenMod    0x0001        // Module: Generate code for the module, if true

#define IsTagField    0x0010        // FieldNode: This field is the trait's discriminant tag
#define IsMixin       0x0020        // FieldNode: Is a trait mixin, vs. an instantiated field

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

#define FlagFirstAssign 0x0080      // VarNameUse: assignment target held no prior value

// Flags used across all types
#define MoveType           0x0001  // Type's values impose move semantics (vs. copy)
#define ThreadBound        0x0002  // Type's value copies must stay in the same thread (vs. sendable)
#define OpaqueType         0x0004  // Type cannot be instantiated as a value (opaque struct, fn, abstract trait)
#define ZeroSizeType       0x0008  // Type has no size in memory (void, empty struct)

#define TraitType          0x0010  // Is a trait (vs. struct)
#define SameSize           0x0020  // An enumtrait, where all implementations are padded to same size
#define HasTagField        0x0040  // A trait/struct has an enumerated field identifying the variant type
#define NullablePtr        0x0080  // trait/struct has nullable pointer, generating optimized data

// Type check progress, carried by every declaration. These are type check's
// marks and no other phase's: inodeTypeCheck sets and tests them, and neither
// name resolution nor flow analysis reads or writes them.
//
// TypeChecking means a declaration's type check has begun: it can still answer
// what it has already established, which for a type is its identity but not yet
// its size. TypeChecked means its type check finished -- for a nominal type that
// is *laid out*, set before its methods are checked so that a method may use its
// own type by value. See design/type-check-phase.md.
#define TypeChecked        0x8000  // Type check of this declaration finished
#define TypeChecking       0x4000  // This declaration's type check has begun

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

// Obtain name from a named node
Name *inodeGetName(INode *node);

// Determine whether a named node is marked as private
int inodeIsDcl(INode *node);
int inodeIsPrivate(INode *node);

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
