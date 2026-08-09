
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

## FnOverloadDcl

`FnOverloadDclNode` (see `ir/stmt/fndcl.h`) is the namespace binding for a name
declared by more than one function or method. It holds only the common `INode`
header, its `namesym`, and an ordered `Nodes *overloads` vector of the concrete
`FnDclNode` candidates. It has no function type, body, LLVM value, or generated
symbol: every executable implementation remains a separate `FnDclNode`, and a
call is always lowered to the concrete node that was selected.

Because the module or type that owns the candidates already walks each concrete
`FnDclNode`, name resolution and type checking of an `FnOverloadDclNode` are
explicit no-ops; walking its vector again would process the same function
bodies twice.

