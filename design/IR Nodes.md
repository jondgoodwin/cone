
Internal representation (IR) nodes are central to all passes:
1. The [[parser]] creates them while digesting all source files. 
2. [[Name Resolution]] and [[Type Check]] iteratively visit all nodes, and mutate as needed
3. [[Generation]] uses the mutated nodes to generate object files
## Node Types

| Grp  | NodeTag      | Description                                                 |
| ---- | ------------ | ----------------------------------------------------------- |
| Stmt | Keyword      | Lexer-only node for name table consistency                  |
|      | Intrinsic    | Function intrinsic "dcl"                                    |
|      |              |                                                             |
|      | [[Return]]   | `return`                                                    |
|      | [[BlockRet]] | injected last stmt of block lacking return, break, continue |
|      | [[Break]]    | 'break'                                                     |
|      | [[Continue]] | `continue`                                                  |
|      |              |                                                             |
|      | Swap         | `<=>` swap operator                                         |
|      | Import       | `import` command                                            |
|      |              |                                                             |
|      | NameUse      | A name reference (pre-name resolution)                      |
|      | TupleTag     | A comma-separated tuple (pre-name resolution)               |
|      | StarTag      | `*` prefix operator (pre-name resolution)                   |
|      |              |                                                             |
|      | Module       | Module declaration                                          |
|      | FnDcl        | Function/method declaration `fn`                            |
|      | FnOverloadDcl | Overloaded function/method declarations sharing one name   |
|      | VarDcl       | Variable declaration (global, local, parm)                  |
|      | FieldDcl     | Field declaration in a struct, etc.                         |
|      | ConstDclTag  | Constant declaration                                        |
| Exp  | VarNameUse   | Variable or function name use                               |
|      | MbrNameUse   | Member of a type's namespace (field/method)                 |
|      | NilLit       | 'nil' literal (of void type)                                |
|      | ULitTag      | Integer literal                                             |
|      | FLitTag      | Float literal                                               |
|      | StringLit    | String literal                                              |
|      | ArrayLit     | Array literal                                               |
|      | TypeLit      | Type literal                                                |
|      | VTuple       | Value tuple (comma-separated values)                        |
|      | Assign       | Assignment expression                                       |
|      | FnCall       | Function/method call or field access                        |
|      | ArrIndex     | Array index                                                 |
|      | FldAccess    | Struct field access                                         |
|      | Sizeof       | size of a type                                              |
|      | Cast         | Cast exp to another type                                    |
|      | Borrow       | & (address of) operator                                     |
|      |              |                                                             |
  

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

Types of IR Nodes:
- Expression Nodes
	- [[fncall]] 

## Common IR Node fields

All IR node types start with these common fields (see inodes.h):

| Name     | Type     | Purpose                                             |
| -------- | -------- | --------------------------------------------------- |
| instnode | Inode*   | Generic or macro that created instance (or NULL)    |
| lexer    | Lexer*   | contains -> url (filepath) and -> source            |
| srcp     | char*    | Points to start of parsed token in source file info |
| linep    | char*    | Points to start of line containing srcp token       |
| linenbr  | uint32_t | line number in source file                          |
| tag      | uint16_t | Encoded node type+flags (see NodeTags and below)    |
| flags    | uint16_t | compile-time flags                                  |

## The three type sentinels, and what tells them apart

`corelib.c` builds three singletons that are all `AbsenceNode`s retagged
`UnknownTag`. They are distinguished by **identity**, never by tag, so a
comparison against one must be `==` and never a tag test:

| Singleton | Means | Effect on diagnostics |
| --- | --- | --- |
| `unknownType` | not inferred yet | none — a type may still be worked out |
| `noCareType` | the receiver does not care what type comes back | none |
| `errorType` | already reported as bad | **suppresses** follow-on complaints |

Conflating `errorType` with `unknownType` buys either spurious cascades or masked
real failures, which is why they are separate objects sharing a tag rather than
one object.

`errorType` is installed by an error path that has reported a diagnostic and has
no honest type to give the node. `itypeMatches` returns `EqMatch` when either
side is `errorType`, and `iexpMultiInfer` skips a branch carrying it, so
everything derived from an already-reported node stays quiet. `inodeIsError`
(`inode.c`) is the predicate for "an earlier diagnostic already covered this".

Where a whole subtree has to be replaced rather than one node retyped,
`newErrorNode` (`void.c`) builds an expression-shaped stand-in: a fresh
`AbsenceNode` carrying the replaced expression's source position, with `errorType`
as its value type. It is per-site rather than a singleton precisely because it
holds a position.

**Every constructor of an expression node must initialize `vtype`.** `memAllocBlk`
does not zero, so an unset `vtype` holds arena garbage rather than NULL — which is
how one of these defects faulted at an arbitrary address instead of at zero, and
why `--checktree` could not see it. A node that has not been type checked yet
carries `unknownType`.

## --checktree

`ir/checktree.c` walks the program after semantic analysis and reports
`ErrorBadTree` for any expression node with no value type or any block with no
statement list. It runs whether or not errors were reported, because a phase that
reports a bad program and returns early is exactly what leaves those holes.
`test/run.py` passes `--checktree` on every compile of every category.

It does not descend into type declarations: a type may refer to itself through a
reference, so that graph has cycles, and a type node's own `vtype` is not what
these defects leave empty.

## FnOverloadDcl

`FnOverloadDclNode` (see `ir/stmt/fndcl.h`) is the namespace binding for an
explicitly declared overload name. It holds only the common `INode` header, its
`namesym`, and an ordered `Nodes *overloads` vector of the concrete `FnDclNode`
candidates that declared they overload that name. It has no function type, body,
LLVM value, or generated symbol: every executable implementation remains a
separate `FnDclNode` bound to its own unique concrete name, and a call is always
lowered to the concrete node that was selected.

`FnDclNode` carries `Name *overloadsym`, which is the overload name this
declaration also answers to, or `NULL` when it has only its concrete name. That
field is what lets a cloned method (a generic instantiation or an inherited trait
default) rebuild its overload membership in the cloned type's namespace, since
the overload node itself is never copied.

Name resolution of an `FnOverloadDclNode` is an explicit no-op, because the
module or type that owns the candidates already walks each concrete `FnDclNode`;
walking its vector again would resolve the same function bodies twice. Type
checking only compares the candidates' already-checked signatures, to report two
candidates that would accept the same arguments.

