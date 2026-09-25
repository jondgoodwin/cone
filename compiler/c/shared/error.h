/** Error handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef error_h
#define error_h

typedef struct INode INode;    // ../ast/ast.h

// Exit error codes
//
// Every value is explicit. These numbers are a published interface: the test
// suite's scenarios name them symbolically but match the compiler's output on
// the number, so an inserted code that renumbered everything below it would
// silently invalidate every expectation at once. Add new codes at the end of
// their block with the next free number. Never renumber an existing one.
enum ErrorCode {
    // Terminating errors
    ExitSuccess = 0,
    ExitError = 1,    // Program fails to compile due to caught errors
    ExitNF = 2,        // Could not find specified source files
    ExitMem = 3,    // Out of memory
    ExitOpts = 4,    // Invalid compiler options
    // 5 was ExitIndent, the lexer's block-stack overflow; there is no block stack
    // Unrecoverable internal failure: code generation could not proceed, or a
    // compiler invariant did not hold. Both mean the compiler has nothing
    // further it can honestly say about this source, so both stop here rather
    // than accumulate. See errorUnreachable.
    ExitGen = 6,

    // Non-terminating errors
    ErrorCode = 1000,
    ErrorBadTok = 1001,    // Bad character starting an unknown token
    ErrorGenErr = 1002,    // Failure to perform generation activity
    ErrorNoSemi = 1003,    // Missing semicolon
    ErrorNoLCurly = 1004,  // Missing left curly brace
    ErrorNoRCurly = 1005,    // Missing right curly brace
    ErrorBadTerm = 1006,   // Invalid term - something other than var, lit, etc.
    ErrorBadGloStmt = 1007, // Invalid global area type, var or function statement
    // 1008 was ErrorDupImpl; a second implementation of a name is ErrorDupName
    ErrorNoLParen = 1009,  // Expected left parenthesis not found
    ErrorNoRParen = 1010,    // Expected right parenthesis not found
    ErrorDupName = 1011,    // Duplicate name declaration
    ErrorNoName = 1012,    // Name is required but not provided
    ErrorInvType = 1013,    // Types do not match correctly
    ErrorNoIdent = 1014,    // Identifier expected but not provided
    ErrorNotLit = 1015,    // Value can only be a literal
    ErrorBadLval = 1016,    // Expression is not an lval
    ErrorNoMut = 1017,        // Mutation is not allowed
    // 1018 was ErrorNotFn; calling a non-callable value is ErrorNoMbr on '()'
    ErrorUnkName = 1019,    // Unknown name (no declaration exists)
    ErrorNoType = 1020,    // No type specified (or inferrable)
    ErrorNoInit = 1021,    // Parm didn't specify required default value
    ErrorFewArgs = 1022,    // Too few arguments specified
    ErrorManyArgs = 1023,    // Too many arguments specified
    ErrorNoMbr = 1024,        // Method/field not specified
    ErrorNoMeth = 1025,    // No such method defined by the object's type
    ErrorRetNotLast = 1026, // Return was found not at the end of the block
    ErrorNoRet = 1027,        // Return value expected but not given
    ErrorNoElse = 1028,    // 'if' used as a value has no 'else' (or exhaustive matches)
    ErrorNoLoop = 1029,    // 'break' or 'continue' allowed only in while/each loop
    // 1030 was ErrorNoVtype; a node left with no type is ErrorBadTree (--checktree)
    ErrorNotPtr = 1031,    // Not a pointer
    // 1032 was ErrorNotLval, 1033 ErrorAddr; both conditions are ErrorBadLval
    ErrorBadPerm = 1034,    // Permission not allowed
    // 1035 was ErrorNoFlds; a type with no such field is ErrorNoMbr
    ErrorBadAlloc = 1036,  // Region cannot allocate: missing or ill-formed alloc method
    // 1037 was ErrorNoDbl, for a missing '::'; the language has no '::'
    ErrorNoVar = 1038,        // Missing variable name
    ErrorNoEof = 1039,        // Missing end-of-file
    ErrorNoImpl = 1040,    // Function must be implemented
    ErrorBadImpl = 1041,    // Function must not be implemented
    ErrorNotPublic = 1042, // Private name accessed from outside what may see it
    ErrorBadMeth = 1043,   // Methods/fields not supported
    ErrorNotTyped = 1044,  // Expected a value that has a type
    ErrorBadIndex = 1045,  // Bad index/slice on array/ref
    ErrorBadArray = 1046,  // Bad array
    // 1047 was ErrorBadSlice; borrowing a slice of a non-array is a one-element
    // slice by design (borrow.c), so there is no bad-slice-type condition
    ErrorMove = 1048,      // Move error of some kind
    // 1049 was ErrorRecurse; a type reached mid-layout has no size, which is
    // ErrorNoSize, and recursion through a reference is legal
    ErrorBadStmt = 1050,   // Bad statement
    ErrorBadElems = 1051,  // Inconsistent tuple elements

    // Overloaded function/method declaration and selection
    ErrorBadOverload = 1052,    // Malformed 'overload' declaration
    ErrorGenericOverload = 1053,// Generic function may not also declare an overload name
    ErrorOverloadClash = 1054,  // Overload name is already bound to a different kind of declaration
    ErrorDupOverload = 1055,    // Two candidates accept the same parameter signature
    ErrorNoCandidate = 1056,    // No overloaded candidate accepts the call's arguments
    ErrorAmbigCandidate = 1057, // More than one overloaded candidate accepts the call's arguments
    ErrorOverloadUse = 1058,    // Overload name used somewhere other than a call's callee
    ErrorPrivOverload = 1076,   // A private candidate may not join a public overload name

    // Array literals
    ErrorBadFill = 1059,        // Array fill literal may not repeat this value
    ErrorFillCount = 1060,      // Array fill literal's element count cannot be counted into

    // Generics
    ErrorNoGenParms = 1061,     // Type parameter list declares no parameters
    ErrorGenParmConstr = 1086,  // Type parameter carries a constraint or a type, neither implemented

    // Iteration
    ErrorNotIterable = 1062,    // Value cannot be iterated over by 'each'

    // IR well-formedness (--checktree). A compiler defect, not a bad program
    ErrorBadTree = 1063,        // A node was left with no type or no body

    // Lifetimes
    ErrorEscape = 1064,         // A borrowed reference would outlive what it borrows from
    ErrorCallEscape = 1085,     // A call could store a borrowed-reference argument where it would outlive what it points at

    // Words and spellings held for language features not yet implemented
    ErrorReserved = 1065,       // A reserved word used as an identifier; '?.'; a '#' word

    // Reinterpretation, checked where the size is known
    ErrorRecastSize = 1066,     // 'as' onto a struct whose size differs from the source's

    // Generic and macro expansion
    ErrorInstDepth = 1067,      // Expansion nested deeper than the compiler will follow

    // Demand-driven analysis
    ErrorCircular = 1068,       // A declaration whose type comes from a value that names it back,
                                // or two types that each extend or mix in the other

    // Layout
    ErrorNoSize = 1069,         // A value whose type cannot report a size

    // Argument lists, split out of ErrorManyArgs, which now means only that a
    // call passed more arguments than the declaration accepts
    ErrorArgCount = 1070,       // An instantiation's argument count is not its parameter count
    ErrorNotType = 1071,        // A name where a type must be that names no type: a generic argument, a pattern naming a value, a module or module trait
    ErrorNoArgs = 1072,         // A generic or macro with parameters was named without arguments
    ErrorFldArgs = 1073,        // Arguments given to a field access, which accepts none

    // Reference types
    ErrorNoRefType = 1074,      // A reference type did not name what it refers to
    ErrorInlineRef = 1083,      // A borrow of an inline function, which has no code of its own to point at
    ErrorNoRead = 1084,         // A read through a reference whose permission grants no read

    // The compiler's own invariants. This is the one code no source is supposed
    // to be able to produce, and so the one code with no scenario: reaching it
    // means a compiler defect, not a bad program. See errorUnreachable.
    ErrorUnreachable = 1075,    // A state the compiler had established cannot happen

    // Macro methods
    ErrorBareMbr = 1077,        // A macro method's body names a member of its type without 'self.'

    // Syntax the language no longer has
    ErrorColonBlock = 1078,     // ':' where a block should start; indentation does not delimit a block

    // Visibility
    ErrorBadPub = 1079,         // 'pub' on something that has no namespace to be visible outside of

    // Storage
    ErrorBadStatic = 1080,      // 'static' where there is no per-instance copy to share: not a variable, or in an inline fn

    // Name folding
    ErrorBadFold = 1081,        // A 'use' clause where none may be written, or naming what cannot fold

    // Lexer
    ErrorLitOverflow = 1082,    // An integer literal whose digits do not fit in 64 bits

    // Virtual dispatch
    ErrorGenericVtable = 1087,  // A trait requiring a generic method, which has no one signature a vtable slot could hold

    // The closed family: 'enum'
    ErrorEnumAbstract = 1088,   // 'enum trait': an enum's variant set is its identity, so no abstraction corresponds to one
    ErrorOpenTrait = 1089,      // Variants declared inside a trait, which is open; a closed set is an enum
    ErrorVariantDcl = 1090,     // A variant restating what its enum decides: its base, or its type parameters
    ErrorDupTag = 1091,         // Two variants holding the same tag value
    ErrorTagWidth = 1092,       // A tag value too large for the integer type the enum declared
    ErrorBadUnsized = 1093,     // '@unsized' where there is no variant padding to decline
    ErrorNoVariants = 1094,     // An enum declaring no variants
    ErrorEnumEquality = 1095,   // '==' on an enum whose variants carry payloads, which have no comparison

    // Nominal is-a conformance, asserted with 'is'
    ErrorExtends = 1096,        // A base clause that may not stand where it is written: 'extends' on an abstraction, or either clause twice
    ErrorIsaFields = 1097,      // A trait's fields not declared by the type, in the trait's order, at position 0
    ErrorIsaMulti = 1098,       // A trait after the first in an 'is' list requiring fields, which only the first may

    // 'trait' as a modifier on the kind
    ErrorDupTrait = 1099,       // 'trait' written twice: by itself it already means 'struct trait'
    ErrorUnbuiltKind = 1100,    // A kind of declaration the grammar admits and the compiler does not build: 'actor', 'actor trait'. Also the retired in-file 'mod name { }' block, recognised only to refuse it

    // 'extends': enriching a concrete type with methods
    ErrorExtendsBase = 1101,    // What an 'extends' names cannot serve as a concrete base
    ErrorExtendsField = 1102,   // A field declared by a type that extends a concrete base, which may add none
    ErrorExtendsOverride = 1103,// A name of the base redeclared by the enriching type, enum extension or module, which may not override

    // A type body's 'use': folding a sibling enrichment's methods in
    ErrorUseSibling = 1104,     // A type-body 'use' naming what is not a sibling of this type, or a member that does not fold from one

    // 'extends': an enum adding variants to another enum's variant set
    ErrorEnumExtends = 1105,    // What an enum's 'extends' names cannot be its base, or what such an enum's body may not declare: a requirement, a common field, a discriminant, a macro or a mixin
    // 1106 was ErrorEnumExtendsSize; an extension's variants are copies with their own layout, so an added one may be any size

    // 'mod': the declaration that names a file's module
    ErrorModDcl = 1107,         // A 'mod' declaration where the module is already established: not its file's first statement, or a second one

    // A module's 'use' on a global: folding a singleton's members in as its own names
    ErrorUseGlobal = 1108,      // A global whose type cannot be a fold's source: not a struct, or an abstraction

    // The folder sweep: a module's files are the files of its folder
    ErrorModName = 1109,        // A 'mod' declaration naming something other than the module's folder, or a one-file submodule's file, which is what names it
    ErrorDupFile = 1110,        // Two files of one module sharing a basename, which leaves neither nameable
    ErrorModFile = 1111,        // A file brought into a module that another module already holds, or that this one already swept in

    // Folder modules: a subfolder holding its own designated file is a submodule
    ErrorModFolder = 1112,      // A designated file, or a one-file module, beneath an organisational folder, which is no module: a module sits directly in its parent module's folder

    // Import within the tree: a module reaches its neighbours through the registry its parent is
    ErrorModReach = 1113,       // An 'import' walking a path to a file inside a module tree, or a module naming its own parent: a module in a tree is reached by name, never by a path that happened to arrive at it. Also a standalone 'use' naming a module that is not a submodule of this one: itself, an ancestor, or one beside it

    // 'include', retired: a module's files are its folder's files
    ErrorInclude = 1126,        // An 'include' statement: a file joins a module by being in its folder, so nothing brings one in

    // A module's standalone 'use': folding an enum's variants, or a submodule's names, in as names of the module
    ErrorUseEnum = 1114,        // A module's standalone 'use' naming neither an enum declaration nor a module: another kind of name, a typedef, or an instance of a generic enum

    // A pattern's bare variant, looked up in the matched value's enum
    ErrorPatArgs = 1115,        // A pattern's variant found only in the matched value's enum, written with type arguments that value supplies

    // One import of a module per module: a second is refused, identical or not [Jon 23 Sep]
    ErrorDupImport = 1116,      // A second import of one module: the same import again, or one that differs in what its 'use' clause folds or in its 'pub'; likewise a second standalone 'use' of one submodule

    // A match's patterns: 'is', comparison and range patterns joined by 'or'
    ErrorPatBare = 1117,        // A value alone where a match expects a pattern, after an 'or': whether a bare value means '==' is not decided

    // A path through an abstraction: 'Trait.name', 'Enum.name'
    ErrorAbstractMeth = 1118,   // A trait's or enum's method named through it, which owns no code for it: each implementer or variant owns a copy

    // Attributes, which are keywords: '@move', '@opaque', '@unsized'
    ErrorUnkAttr = 1119,        // A '@' word that names no attribute

    // Moves: what a move-typed value may be moved out of
    ErrorMoveOut = 1120,        // A move-typed value moved out through a reference that does not solely own it: a borrowed one, or a shared (aliasable) owning one

    // 'mod A extends B': a module reusing another module's public names
    ErrorModExtends = 1122,     // What a module's 'extends' names cannot be reused: not a module, a trait (conforming to a module trait is not built), the module itself or one it contains, or a path (a loop of 'extends' is ErrorImportLoop)

    // Comparing references: '==' and ordering read through to the values, '===' asks whether they are the same place
    ErrorRefNoCompare = 1123,   // '==', '!=' or an ordering on references whose referent has no such operator, or on a slice or virtual reference, whose referents have no comparison built
    ErrorRefCompareMixed = 1124, // A comparison with a reference on one side and a value on the other: both are read through, or neither
    ErrorSameNotRef = 1125,     // '===' or '!==' on a value that is neither a reference nor a pointer, which has no place to be the same as

    // A module's standalone 'use' folds a namespace reached without an import; an imported module is folded by its import's clause
    ErrorUseImported = 1127,    // A standalone 'use' naming a module this module reaches through an import, whose names its import's own 'use' clause folds

    // One-file modules: a file of a module's folder whose first statement is 'mod' is a submodule of its own
    ErrorModFileFolder = 1128,  // A one-file module beside a module folder of the same name in one parent's folder: 'lexer.cone' declaring 'mod' beside 'lexer/lexer.cone'

    // The build description: the file that says which files make up each module of one package, and where each import is
    ErrorBuildDesc = 1129,      // A build description that is not well formed: a setting, module, file or import line that cannot be read, a name written twice, a module listing no file
    ErrorBuildModName = 1130,   // A 'mod' line naming a module other than the one the build description lists its file in, or imports it as
    ErrorBuildImport = 1131,    // An 'import' in a described module that the build description provides nothing for: the compiler never searches for one

    // 'extern' says a declaration is defined elsewhere; '@c' says its symbol takes C naming
    ErrorCAttr = 1132,          // '@c' written wrongly or where nothing has a symbol for it to name: a bad argument, a type, an anonymous, generic or inline fn, a trait's method, a generic module, the retired 'extern system'
    ErrorCNameTwice = 1133,     // A bare '@c' (or '@c(system)' where the module is already system) on a fn whose module already gives it that C naming
    ErrorBadExtern = 1134,      // 'extern' on a declaration an importer needs the body of -- inline, generic, a trait's or a generic type's method -- or on something not a fn or a global

    // A module trait, and a module conforming to one with 'is'
    ErrorModIs = 1135,          // A 'mod' line's 'is' naming something other than one module trait by one name -- a struct trait, a module, a path, a list -- or written before 'extends'
    ErrorModTraitBody = 1136,   // A module trait's body holding something other than a function or a global: a type, an import, a 'use', a macro, a generic fn, an overload name
    ErrorModTraitMissing = 1137, // A module conforming to a module trait declares nothing under a member's name, and the trait gives that member no default
    ErrorModTraitMismatch = 1138, // What a conforming module declares under a member's name is not the member's kind, signature, type or permission

    // The module order: imports form a DAG at every scale
    ErrorImportLoop = 1139,     // Modules that depend on each other round a loop -- by importing a module or a name of it, by 'extends', or by containing it

    // A module's 'init' and 'final'
    ErrorGlobalUninit = 1140,   // A global declared without an initial value that its module's 'init' never assigns, or whose module has no 'init'
    ErrorModLifecycle = 1141,   // A module's 'init' or 'final' not declared as 'fn @initpure init()' or 'fn final()', or a module's own 'drop' where it needs its finalizer given that name

    // A generic module, 'mod stack[T];'
    ErrorGenModBare = 1142,     // A generic module named without type arguments where only an instance has members: a path through it, an import's 'use' clause or default fold of it, 'extends' or a standalone 'use' naming it
    ErrorGenModBody = 1143,     // A generic module holding what an instance is not yet built for: a submodule, a generic function or type, a trait or an enum, a module trait
    ErrorGenModRoot = 1144,     // An executable's root module declared generic: nothing can instantiate the program

    // A file's header: its 'mod' line, then its imports
    ErrorNoModDcl = 1145,       // A module's first file -- designated, one-file, lone, or first listed in a build description -- not opening with its 'mod' line
    ErrorImportLate = 1146,     // An 'import' after a declaration: imports come right after the 'mod' line, ahead of everything else

    // Generating a library's include file
    // 1147 was ErrorIncSubmodule, a reach into one of the root's submodules; the include file now writes a nested module block for it
    ErrorIncWrite = 1148,       // The include file cannot be written, or would overwrite a source file of the compile
    ErrorIncCheck = 1149,       // The generated include file could not be completed, or does not parse and name-resolve as the package's module: a compiler limitation or bug

    // Warnings
    WarnCode = 3000,
    WarnName = 3001,        // Unnecessary name
    // 3002 was WarnIndent, mixed tabs and spaces; indentation means nothing to the compiler
    WarnCopy = 3003,       // Unsafe attempt to copy a CopyMethod or CopyMove typed value
    WarnLoop = 3004,       // Infinite loop with no break

    // Uncounted
    Uncounted = 9000,
};

extern int errors;

// Send an error message to stderr
void errorExit(int exitcode, const char *msg, ...);
void errorMsgNode(INode *node, int code, const char *msg, ...);
void errorMsgLex(int code, const char *msg, ...);
void errorMsgLexAfter(int code, const char *msg, ...);
void errorMsg(int code, const char *msg, ...);
void errorUnreachable(INode *node, const char *msg);
void errorSummary();

#endif
