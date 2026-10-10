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
    ErrorNoType = 1020,    // No type specified (or inferrable); in the parser, a type the grammar requires left out
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
    ErrorGenericOverload = 1053,// Generic function declares an overload name but a type parameter of it is not bound to a function signature
    ErrorOverloadClash = 1054,  // Overload name is already bound to a different kind of declaration
    ErrorDupOverload = 1055,    // Two candidates accept the same parameter signature
    ErrorNoCandidate = 1056,    // No overloaded candidate accepts the call's arguments
    ErrorAmbigCandidate = 1057, // More than one overloaded candidate accepts the call's arguments
    ErrorOverloadUse = 1058,    // Overload name used somewhere other than a call's callee
    ErrorPrivOverload = 1076,   // A private candidate may not join a public overload name

    // 1059 was ErrorBadFill and 1060 ErrorFillCount, the fill literal '[n; x]' repeating a move value or a counted
    // reference; an array's contents evaluate the value for each element, under the move rule (ErrorFillLiteral)

    // Generics
    ErrorNoGenParms = 1061,     // Type parameter list declares no parameters
    ErrorGenParmConstr = 1086,  // A parameter's annotation that is not built: on a generic's, one naming no trait (a value or kind parameter, or a generic trait given no type arguments); on a macro's or generic module's, any

    // Iteration
    ErrorNotIterable = 1062,    // Value cannot be iterated over by 'each'

    // IR well-formedness (--checktree). A compiler defect, not a bad program
    ErrorBadTree = 1063,        // A node was left with no type or no body

    // Lifetimes
    ErrorEscape = 1064,         // A borrowed reference would outlive what it borrows from
    ErrorCallEscape = 1085,     // A call could store a borrowed-reference argument where it would outlive what it points at
    ErrorFrozen = 1168,         // A borrowed source read, changed, moved, borrowed or ended while a borrow of it that forbids that is still to be used

    // Words and spellings held for language features not yet implemented
    ErrorReserved = 1065,       // A reserved word used as an identifier; '?.'; a '#' word

    // Reinterpretation, checked where the size is known
    ErrorRecastSize = 1066,     // 'as' onto a struct whose size differs from the source's

    // Generic and macro expansion
    ErrorInstDepth = 1067,      // Expansion nested deeper than the compiler will follow

    // Demand-driven analysis
    ErrorCircular = 1068,       // A declaration whose type comes from a value that names it back,
                                // a constant whose value reaches itself through named constants,
                                // or two types that each extend or name the other in an 'is'

    // Layout
    ErrorNoSize = 1069,         // A value whose type cannot report a size

    // Argument lists, split out of ErrorManyArgs, which now means only that a
    // call passed more arguments than the declaration accepts
    ErrorArgCount = 1070,       // An instantiation's argument count is not its parameter count
    ErrorNotType = 1071,        // A name where a type must be that names no type: a generic argument, a pattern naming a value, a module or module trait
    ErrorNoArgs = 1072,         // A generic or macro with parameters was named without arguments
    ErrorFldArgs = 1073,        // Arguments given to a field access, which accepts none
    ErrorNamedArg = 1165,       // A 'name: value' argument anywhere but a type literal: a call, an index or a macro use

    // Reference types
    ErrorNoRefType = 1074,      // A reference type did not name what it refers to
    ErrorInlineRef = 1083,      // A borrow of an inline function or an intrinsic, which has no code of its own to point at
    ErrorNoRead = 1084,         // A read through a reference whose permission grants no read

    // Regions: the annotation struct after '+', and the 'RegionRef' methods the compiler calls on it
    ErrorNotRegion = 1154,      // A reference's region names a struct that does not declare 'is RegionRef'
    ErrorRegionMeth = 1155,     // A region's 'aliasRef', 'dealiasRef', 'free' or 'mark' not of the shape the compiler calls it with
    ErrorRegionSet = 1156,      // A region whose declaration contradicts itself: 'is Move' (one owner per value) with an 'aliasRef' method (another owner), or with 'Traced' (a collector for a value one owner frees)
    ErrorRegionRefUse = 1157,   // 'RegionRef' anywhere but a struct's 'is' list: the type a reference points at, or the 'is' of a trait, enum or variant

    // Traced regions: a region ref declaring 'Traced', and where its references may be held (ir/types/region.c)
    ErrorTracedUse = 1169,      // 'Traced' declared by a type that is not a region ref: a struct not declaring 'RegionRef', a trait, an enum or a variant
    ErrorTracedMark = 1170,     // A region declaring 'Traced' with no 'mark' method for its trace to call
    ErrorTracedHeld = 1171,     // A traced reference held inside what a region that is not traced allocates: 'Rc[T]', 'So[T]' with T holding one
    ErrorTracedGlobal = 1172,   // A global or static whose type holds a traced reference, which no collector finds
    ErrorTracedBorrow = 1173,   // A traced region's reference to a value that holds a borrowed reference, which its trace cannot see and no lifetime covers
    ErrorTracedRaw = 1174,      // mem.writeRaw or mem.moveRaw of a type holding a traced reference: placing one in raw memory no collector traces (an arena's, a pool's, a collection's)
    ErrorTracedRefKind = 1175,  // An owning virtual reference into a traced region, whose trace could not find its header
    ErrorTracedPerm = 1176,     // A traced region's reference whose permission is a lock permission, which a traced region does not take: its lock would sit where the collector finds the value, and no owner is counted for a borrow's guard

    // Intrinsics: '@intrinsic' declarations, checked against the compiler's registry (ir/stmt/intrinsic.c)
    ErrorIntrinsicPlace = 1160, // '@intrinsic' on a function not of the core package -- of its root, a submodule, or a plain struct one declares: another package's, a method taking 'self', a generic type's or a trait's
    ErrorIntrinsicName = 1161,  // '@intrinsic' on a function whose name the compiler's registry does not define
    ErrorIntrinsicSig = 1162,   // An intrinsic declared with a signature other than the registry's: type parameters, parameters or return type
    ErrorIntrinsicBody = 1163,  // A body written for an intrinsic the registry gives no fallback, or none where it has no lowering to use instead
    ErrorIntrinsicType = 1164,  // An intrinsic instantiated at a type outside its type class: one with no size, or for an atomic operation one that is not an integer of 8 to 64 bits, bool or a raw pointer, as the operation allows

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
    ErrorFloatRange = 1159,     // A float literal whose value is past its type's range: f32 unless suffixed 'd' or 'f64'

    // Literals: a value and the type it is built at
    ErrorLitRange = 1158,       // An integer literal whose value does not fit its type: its suffix's, the one it is given, or the i32 it defaults to

    // Virtual dispatch
    ErrorGenericVtable = 1087,  // A trait requiring a generic method, which has no one signature a vtable slot could hold
    ErrorPrivateVtable = 1258,  // A trait's private method or field reached through a virtual reference: the vtable holds public members only

    // The closed family: 'enum'
    ErrorEnumAbstract = 1088,   // 'enum trait': an enum's variant set is its identity, so no abstraction corresponds to one
    ErrorOpenTrait = 1089,      // Variants declared inside a trait, which is open; a closed set is an enum
    ErrorVariantDcl = 1090,     // A variant restating what its enum decides: its base, or its type parameters
    ErrorDupTag = 1091,         // Two variants holding the same tag value
    ErrorTagWidth = 1092,       // A tag value its discriminant cannot hold: outside the integer type the enum declared or the one its base settled, or none holds it
    ErrorBadUnsized = 1093,     // '@unsized' where there is no variant padding to decline
    ErrorNoVariants = 1094,     // An enum declaring no variants
    ErrorEnumEquality = 1095,   // '==' on an enum with a variant that carries fields and declares no '=='

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
    ErrorEnumExtends = 1105,    // What an enum's 'extends' names cannot be its base, or what such an enum may not declare: a requirement, a common field, a discriminant, a macro, or an 'is' of its own
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
    ErrorUseEnum = 1114,        // A module's standalone 'use' naming neither an enum declaration nor a module: another kind of name, an 'alias' of a type, or an instance of a generic enum

    // A pattern's bare variant, looked up in the matched value's enum
    ErrorPatArgs = 1115,        // A pattern's variant found only in the matched value's enum, written with type arguments that value supplies

    // One import of a module per module: a second is refused, identical or not [Jon 23 Sep]
    ErrorDupImport = 1116,      // A second import of one module: the same import again, or one that differs in what its 'use' clause folds or in its 'pub'; likewise a second standalone 'use' of one submodule

    // A match's patterns: values alone, 'is', comparison and range patterns joined by 'or'
    ErrorPatBare = 1117,        // A condition where a match expects a pattern, 'not b' or 'n > 3': a value alone is compared with the matched value, and whether a condition may stand for a pattern is not decided

    // A path through an abstraction: 'Trait.name', 'Enum.name'
    ErrorAbstractMeth = 1118,   // A trait's or enum's method named through it, which owns no code for it: each implementer or variant owns a copy

    // Attributes, which are keywords: '@opaque', '@unsized', '@c', '@initpure', '@intrinsic'
    ErrorUnkAttr = 1119,        // A '@' word that names no attribute

    // Moves: what a move-typed value may be moved out of
    ErrorMoveOut = 1120,        // A move-typed value moved out through a reference that does not solely own it: a borrowed one, or a shared (aliasable) owning one

    // 'mod A extends B': a module reusing another module's public names
    ErrorModExtends = 1122,     // What a module's 'extends' names cannot be reused: not a module, a trait (conforming to a module trait is not built), the module itself or one it contains, or a path (a loop of 'extends' is ErrorImportLoop)

    // Comparing references: '==' and ordering read through to the values, '===' asks whether they are the same place
    ErrorRefNoCompare = 1123,   // '==', '!=' or an ordering on references whose referent has no such operator; on slices whose elements have no '==', or an ordering on slices, which have none; or on virtual references, whose referents have no comparison built
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
    ErrorCAttr = 1132,          // '@c' written wrongly or where nothing has a symbol for it to name: a bad argument, a type, an anonymous, generic or inline fn, an intrinsic, a trait's method, a generic module, the retired 'extern system'
    ErrorCNameTwice = 1133,     // A bare '@c' (or '@c(system)' where the module is already system) on a fn whose module already gives it that C naming
    ErrorBadExtern = 1134,      // 'extern' on a declaration an importer needs the body of -- inline, generic, an intrinsic, a trait's or a generic type's method -- or on something not a fn or a global
    ErrorCNameConflict = 1152,  // One C name declared two ways in one compile: functions of differing signatures, globals of differing type or permission, a function and a global, or a symbol the compiler generates itself
    ErrorCNameDefTwice = 1153,  // One C name defined twice in one compile: two bodies the linker would see, or two defined globals

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
    ErrorImportLate = 1146,     // An 'import' not right after the 'mod' line: after a declaration, or in a module's file that has no 'mod' line

    // Generating a library's include file
    // 1147 was ErrorIncSubmodule, a reach into one of the root's submodules; the include file now writes a nested module block for it
    ErrorIncWrite = 1148,       // The include file cannot be written, or would overwrite a source file of the compile
    ErrorIncCheck = 1149,       // The generated include file could not be completed, or does not parse and name-resolve as the package's module: a compiler limitation or bug

    // Enum methods
    ErrorEnumValueDispatch = 1150,  // An enum's method called on a value of the enum: the variant's would be chosen by the tag, which is not built for a value receiver

    // 'mixin', retired: conformance is declared with 'is' on the declaration line
    ErrorMixin = 1151,          // A 'mixin' statement in a type's body: what it took in is named in the 'is' list of the type, the enum or the variant

    // Moving out of a field
    ErrorMoveField = 1166,      // A move-typed value moved out of a field -- a struct's or a tuple's, or what an owning reference held in one points at -- which would leave the struct with a hole in it

    // The built-in traits 'Move' and 'Copy'
    ErrorCopyMove = 1167,       // 'is Copy' on a type that moves: it declares 'Move' too, or has a 'final', a field that moves, or a base that moves

    // The orderings an atomic intrinsic of core's 'mem' is given (ir/stmt/intrinsic.c, intrinsicCallCheck)
    ErrorAtomicConst = 1177,    // An ordering that is not a constant where the call is written: not a MemOrder variant's literal, nor a const holding one
    ErrorAtomicOrder = 1178,    // An ordering the operation forbids: a load's Release or AcqRel, a store's Acquire or AcqRel, a compareSwap failure's Release or AcqRel or one stronger than its success

    // '@threadlocal' globals
    ErrorThreadLocalPlace = 1179, // '@threadlocal' anywhere but after a module global's permission: on a local, a static, a parameter, a field, a module trait's global, a function or a type, or before the permission
    ErrorThreadLocalImm = 1180, // '@threadlocal' on an 'imm' global: a copy per thread of a value no thread can change is the same as one copy
    ErrorThreadLocalInit = 1181, // A '@threadlocal' global, not 'extern', with no initial value: every thread's copy starts from it, and a module's 'init' runs on one thread
    ErrorThreadLocalFinal = 1182, // A '@threadlocal' global whose type needs finalizing: nothing yet runs a finalizer as a thread ends

    // The built-in marker 'AtomicValue' (ir/types/struct.c, structAtomicValueCheck; ir/stmt/const.c)
    ErrorAtomicValueShape = 1183, // 'AtomicValue' declared by something other than a struct of exactly one field: a trait, an enum, a variant, or a struct of no fields or several
    ErrorAtomicValueType = 1184, // An atomic value's one field of a type no atomic operation acts on: not an integer of 8 to 64 bits, a bool or a raw pointer
    ErrorAtomicValueConst = 1185, // A 'const' whose type holds an atomic value: each use of a const is a fresh copy, which no atomic operation could share

    // Generic constraints: 'where T is Name and ... or ...', and '[T Name + Name]' (parser/parsefnflow.c, parseWhere; ir/meta/generic.c)
    ErrorWhereUnmet = 1186,     // An instance of a generic function or type whose type arguments do not meet one of its constraints
    ErrorWhereAbsent = 1187,    // A method or function of a generic type's instance that does not exist there, its 'where' clause unmet
    ErrorWhereForm = 1188,      // A 'where' clause not of the form built: 'T is Name', '+'-joined traits, clauses joined by 'and' and 'or', grouped by parentheses
    ErrorWhereSubject = 1189,   // A 'where' clause's subject that is not a type parameter of the generic or of the generic type it is a member of
    ErrorWhereTrait = 1190,     // What a 'where' clause's subject 'is' names no trait: a generic type, or a generic trait given no type arguments
    ErrorWhereNoParms = 1191,   // A 'where' clause on a declaration with no type parameters to constrain: not generic, nor a member of a generic type
    ErrorGenParmOr = 1192,      // 'or' after a type parameter's traits, '[T A or B]': those are all required, and a choice is written in a 'where' clause

    // The built-in marker 'ThreadSafe' (ir/types/region.c, regionThreadSafeUseCheck)
    ErrorThreadSafeUse = 1193,  // 'ThreadSafe' declared by a type that is not a region ref: a struct not declaring 'RegionRef', a trait, an enum or a variant

    // The thread check: the built-in marker 'Sendable' (ir/meta/generic.c, ir/itype.c, ir/types/reference.c)
    ErrorNotSendable = 1194,    // An instance whose 'T is Sendable' is unmet: the argument holds a borrow, a raw pointer, a non-race-safe owner, an owner of a region not declaring ThreadSafe, or a traced reference

    // The 'Never' return type (ir/stmt/fndcl.c, ir/stmt/return.c)
    ErrorNeverReturns = 1195,   // A function returning 'Never' that can return: it does not end in a call that does not return, or it says 'return'

    // A match's value alone (ir/exp/cast.c)
    ErrorPatType = 1196,        // A type alone as a match pattern, other than a variant of the matched value's enum named bare: a value alone is compared with '==', and narrowing is written 'is'

    // The alias statement (parser/parsemod.c, parser/parsetype.c)
    ErrorTypedef = 1197,        // A 'typedef' statement: an alias is declared 'alias Name = type;'
    ErrorAliasEq = 1198,        // An 'alias' statement with no '=' between its name and its target

    // The managed reference type, 'Rc[mut, Node]' (ir/exp/fncall.c, ir/types/reference.c)
    ErrorRefTypeArgs = 1199,    // A region given other than one or two arguments as a reference type: an optional permission, then the value type
    ErrorRefTypePerm = 1200,    // A region's reference type whose first of two arguments is not a permission, or whose value type is one
    ErrorPermNotRegion = 1201,  // A permission given as an argument to a type that is not a region: only a managed reference type takes one
    ErrorPlusRefType = 1202,    // A single or virtual managed reference type written '+R-perm T' outside a match pattern's root: it is 'R[perm, T]'

    // The owning array reference, '+[]R T', which the language does not have (parser/parseexpr.c)
    ErrorOwnedArrayRef = 1203,  // '+[]', as a type or an allocation: an owned runtime-sized array is a List, shared as 'Rc[List[T]]'; a borrowed slice is '&Array[T]'

    // The array type, 'Array[T, n]' (ir/types/array.c, ir/exp/nameuse.c)
    ErrorArrayTypeOld = 1204,   // '[n; T]' as a type, the old spelling: an array type is 'Array[T, n]'
    ErrorArrayTypeArgs = 1205,  // 'Array' given no size, or used without its brackets: it takes an element type, then one size for each dimension
    ErrorArrayTypeElem = 1206,  // 'Array[...]' whose first argument is not a type: the element type comes first, then the sizes

    // A number's conversion, 'u64.from(count)' (ir/exp/fncall.c, ir/exp/typelit.c)
    ErrorNbrBracket = 1207,     // 'u64[count]': a number type takes no '[...]'; a conversion is the method 'u64.from(count)'
    ErrorNbrFrom = 1208,        // A number type's 'from' named without a call, given other than one value, or given a value that does not convert: bool's takes a number, reference or pointer, every other's a number

    // Construction, 'new Point(1, 2)', and the 'init' that fills a value in place (ir/exp/typelit.c, ir/exp/fncall.c, ir/stmt/fndcl.c, ir/flow.c)
    ErrorStructBracket = 1209,  // 'Point[1, 2]': a struct's value is constructed 'new Point(1, 2)'; an enum's variant keeps the brackets
    ErrorNewType = 1210,        // 'new' given what it cannot construct: not a struct (a number converts with 'from'), an enum's variant, a trait or an enum
    ErrorInitNone = 1211,       // No 'init' takes a construction's arguments, or several the struct declares do: one it declares is preferred to the implicit field-wise one
    ErrorInitCall = 1212,       // A type called without 'new', 'Point(1, 2)', or an 'init' called by name: a value is constructed 'new Point(1, 2)'
    ErrorPermNew = 1213,        // '&new' other than as the 'self' of a method that returns nothing: it is an initializer's reference to memory not yet filled
    ErrorInitDcl = 1214,        // A struct's method named 'init' that does not take 'self &new' first, or returns a value: an init fills its value in place
    ErrorInitSelf = 1215,       // In an init, 'self' used before '*self = value' fills it on every path, used other than through '*self', a field or a method call, or not filled when it returns

    // Allocation, 'new Rc[mut, Node](1)' and 'trynew' (ir/exp/allocate.c, ir/exp/typelit.c, ir/types/reference.c)
    ErrorPlusAlloc = 1216,      // An allocation written with '+', '+Rc 5', '+Rc-mut Node[1]', '?+Rc 5' or '+<Rc-mut Rect[3]': an allocation is written 'new Rc[i32](5)' or 'new Rc[mut, Node](1)', one that may fail 'trynew Rc[i32](5)'
    ErrorTryNewValue = 1217,    // 'trynew' on a type that is no managed reference: only an allocation in a region may fail; a value is constructed with 'new'

    // Contents after '<-': listed values, 'n of x', 'fill x' and 'k: v' (parser/parseexpr.c, ir/exp/contents.c, ir/types/array.c)
    ErrorFillLiteral = 1218,    // The fill literal '[n; x]': an array of n copies is constructed with its contents, 'new Array[T, n] <- fill x'
    ErrorRepeatCount = 1219,    // The count of 'n of x' out of 0 through 4294967295, or, in an array's contents, not a constant
    ErrorArrayContents = 1220,  // An array constructed without contents, with init arguments, or with contents that do not fill it exactly
    ErrorFillSize = 1221,       // 'fill x' on a collection that does not say how full it is: it declares no 'len()' or no 'capacity()'
    ErrorPairAppend = 1222,     // A pair 'k: v' given to a collection whose '<-' takes no key and value, or to an array
    ErrorEntryPlace = 1223,     // 'n of x', 'fill x' or 'k: v' where no '<-' takes it apart: inside a tuple that is itself appended

    // The retired 'into' operator (parser/parseexpr.c)
    ErrorInto = 1224,           // 'x into T': a value converts with 'T.from(x)'; a reference narrows to a variant with a check, by a 'match' or 'if imm x &Variant = &value'

    // Construction, continued (ir/exp/assign.c)
    ErrorInitRecurse = 1225,    // In a declared init, '*self = new T(...)' whose construction selects that same init: self is filled by the fields' names, 'new Point(x: 1, y: 2)'

    // Allocation, continued (ir/exp/typelit.c)
    ErrorAllocValue = 1226,     // An allocation of a type no init constructs (a number, an array, a tuple, an enum or its variant, a reference) given other than one value of it: 'new Rc[i32](5)'
    ErrorNewFinished = 1227,    // 'new Point(p)', p already a Point: a finished value needs no construction; an allocation takes one as its value, 'new Rc[Point](p)'
    ErrorTryNewContents = 1228, // Contents after '<-' on a 'trynew' that is not an array's allocation: allocate, then append on Some (ir/exp/contents.c)

    // Constant expressions, folded where a constant is required (ir/exp/literal.c, litFoldConst)
    ErrorConstOverflow = 1229,  // An operation on constants whose result its type cannot hold: '255u8 + 1u8', a float past its range, a float converted to an integer that cannot hold it
    ErrorConstDivZero = 1230,   // A division or remainder of constants by zero
    ErrorConstShift = 1231,     // A shift of a constant by its type's width or more, or by a negative amount

    // Named lifetimes (ir/types/lifetime.h)
    ErrorLifetimePlace = 1232,  // A lifetime named where none is checked: outside a function's signature or a struct's fields, on a borrow, in a function's brackets, or ''static' inside a parameter's reference
    ErrorLifetimeUndeclared = 1233, // A lifetime a struct's fields or a 'where' clause name that is not declared: a struct's second undeclared name, a borrow of none in a struct declaring lifetimes, a name no type of the signature holds
    ErrorLifetimeArgs = 1234,   // A use naming lifetimes its type does not declare, or not as many as it declares
    ErrorLifetimeOr = 1235,     // A lifetime comparison under 'or' or 'not' in a 'where' clause: lifetimes are never instanced, so only 'and' joins one

    // A type's members (ir/stmt/fielddcl.c)
    ErrorPubFieldPrivType = 1236, // A 'pub' field whose type names a type private to its module: code outside the module would reach into a type it cannot name

    // Literals and implicit coercion
    ErrorNullNotPtr = 1237,     // A 'null' wanted as something other than a raw pointer, or where nothing says which raw pointer type it is
    ErrorBoolNotNbr = 1238,     // A bool where a number is wanted: a bool converts to a number only explicitly, 'T.from(b)'
    ErrorPtrSizedAs = 1257,     // 'as' between usize or isize and a fixed-width number: a pointer's width differs by target, so it converts, 'T.from(x)'

    // Constants of the build (TEMPORARY, a provisional mechanism whose final design is open)
    ErrorDefineName = 1239,     // isDefined or definedInt given a '-D' name that is not a string literal

    // Invariant lifetimes (ir/types/lifetime.h)
    ErrorBrand = 1240,          // A value of one invariant lifetime where another is wanted: another arena's key, an arena and a key of different brands, or a brand lost or gained
    ErrorBrandLoop = 1241,      // A value whose invariant lifetime a loop's pass minted, kept where a later pass or the code after the loop could use it
    ErrorKeyAccess = 1242,      // A key, a reference of an invariant lifetime, dereferenced or reached through: only its arena's '[]' reaches what it names
    ErrorLifetimeInvariant = 1243, // An invariant lifetime ordered by '>=', equated with an ordinary one, named where an ordinary one is declared, on a slice or virtual reference, or not one the function names
    ErrorKeyBorrow = 1244,      // A key to a value that holds a borrow: a value in a dynamic arena outlives every scope

    // Lock permissions: a struct declaring 'LockPermission' in a managed reference's permission slot (ir/types/permission.c, ir/exp/borrow.c)
    ErrorNotLockPerm = 1245,    // A struct in a managed reference's permission slot that does not declare 'is LockPermission'
    ErrorLockPermShape = 1246,  // A lock permission whose methods are not the shape the compiler calls: no 'acquireMut' and 'releaseMut', a read pair half declared, a method taking more than 'self' or returning a value
    ErrorLockRegion = 1247,     // A lock permission on a region it does not fit: one owner ('So'), a region not counting owners, a cross-thread lock on a single-thread region or the reverse, or a virtual reference (a traced region is ErrorTracedPerm)
    ErrorLockAccess = 1248,     // A lock-managed reference read, written or lent without the borrow that takes its lock: '&mut *p', '&*p'

    // Lifetime bounds: '[T + 'a]', 'where T + 'a', '&<Trait + 'a' (ir/types/lifetime.h)
    ErrorLifetimeBound = 1249,  // A lifetime bound not met or not built: a borrow that is not global given for a ''static' bound, a value holding a borrow not known to last ''a' coerced to '&<Trait + 'a', a value holding a borrow that is not global made an owning virtual reference ('So[Trait]', bounded by ''static'), a bound on a generic type's parameter

    // Actors: 'actor Name { ... }' (parser/parseactor.c, ir/types/actor.h). A
    // behaviour's argument that is not Sendable is ErrorNotSendable; the state
    // reached through a handle is ErrorNotPublic
    // 1250 was ErrorActorReturn; a behaviour may return a value, which a reply carries back to an 'await'
    ErrorActorMember = 1251,    // What an actor's body may not hold, or a form of it not built: a 'pub' field, a static, a 'pub' function without 'self', a 'pub fn' method other than 'init', a behaviour without 'self' or named 'init' or 'final', a generic behaviour, a macro, a 'use', an 'extern' or a 'self' of another kind; a generic actor, its 'is', 'extends' or an attribute
    ErrorActorRuntime = 1252,   // An actor declared in a module that does not import the actors package it runs on

    // GPU targets: what a SPIR-V module cannot hold, refused where it is
    // written (ir/flowloan.c, "GPU targets"; genllvm/genllvm.c, genlGpuCalls)
    ErrorGpuRefChoice = 1253,   // A reference, or a value holding one, chosen at run time: an 'if' or 'match' whose arms point at different places, or a holder used after paths that gave it different ones
    ErrorGpuRefIndexed = 1254,  // An array or slice whose elements hold references, indexed by a value known only at run time
    ErrorGpuRefGlobal = 1255,   // A module global or static whose type holds a reference
    ErrorGpuRecursion = 1256,   // A function that calls itself, directly or through others

    // Compute entry points, '@compute(x, y, z)' (ir/stmt/fndcl.c, fnDclComputeCheck;
    // genllvm/genlgpu.c), and what a kernel's slices may come from
    ErrorComputeSize = 1259,    // A workgroup size that is not a constant integer from 1, or one over WebGPU's limits: more than 256 invocations, or z over 64
    ErrorComputeAttr = 1260,    // '@compute' where no entry point can be: a method, a generic, an inline or intrinsic function, one with '@c', an anonymous one; or two entry points of one name
    ErrorComputeSig = 1261,     // An entry point's signature: a return value, a parameter neither Invocation, a slice nor a struct by value, a second Invocation, or more buffers than a kernel binds
    ErrorComputeData = 1262,    // What a kernel's buffer holds: anything but 32-bit numbers and structs and fixed arrays of them
    ErrorComputeTarget = 1263,  // An entry point compiled for SPIR-V's OpenCL form, which has none
    ErrorGpuSliceOrigin = 1264, // In a kernel, an element of a slice or pointer reached by arithmetic from something that is neither a buffer parameter nor a fixed array

    // What a GPU's invocations share: '@workgroup' globals (parser/parsetype.c,
    // ir/stmt/vardcl.c) and atomics (genllvm/genlgpusync.c)
    ErrorWorkgroupPlace = 1265, // '@workgroup' anywhere but after a module global's permission, or beside '@threadlocal'
    ErrorWorkgroupImm = 1266,   // '@workgroup' on an 'imm' global: a workgroup's copy is there for its invocations to change
    ErrorWorkgroupInit = 1267,  // A '@workgroup' global given an initial value: a workgroup's copy starts undefined, and nothing writes a value into it
    ErrorWorkgroupData = 1268,  // What a '@workgroup' global holds: anything but 32-bit numbers, their atomics, and structs and fixed arrays of them
    ErrorGpuAtomicPlace = 1269, // In a kernel, an atomic operation on memory invocations do not share: a local, or a global not '@workgroup'

    // Closures in GPU code, the static form only (ir/exp/closure.c, ir/iexp.c)
    ErrorGpuClosureData = 1298, // A closure literal in GPU code holding what a GPU has none of: a variable it borrows, or a state entry, whose type holds an owning reference, a function or virtual reference, or a raw pointer
    ErrorGpuClosureRef = 1299,  // A closure in GPU code made a function reference, '&fn(...)', or a virtual reference ('&<Trait', an owner of a trait): only a generic bound by a signature takes one there, inlined

    // 'await' in an actor's behaviour (ir/exp/await.c; its seam, ir/flowpath.c)
    ErrorAwaitPlace = 1270,     // 'await' outside an actor's behaviour: in a function, a method of another type, or an actor's 'fn' -- its 'init', its 'final' or a synchronous helper -- which no dispatcher runs as a message
    ErrorUnbuiltAwait = 1271,   // An 'await' every seam rule accepted, where its continuation is not built: one not on a behaviour, one whose record holds a traced reference
    ErrorSelfActorPlace = 1272, // 'selfactor' outside an actor's method: in a function, a method of another type, or an actor's 'final'
    ErrorBehaviourWords = 1273, // 'async' or 'do' without the other, or 'async do' anywhere but in an actor's body: the two words together declare an actor's behaviour, and neither means anything alone
    ErrorAwaitVoid = 1274,      // 'await' on a behaviour that returns nothing, which sends no reply: a caller that needs to know it finished awaits one that declares a return type
    ErrorBehaviourSend = 1275,  // 'self.m()' on one of the actor's own behaviours where it cannot be sent: in its 'final', in a function that is not one of its methods, or through a state that is not the method's own 'self'

    // A condition on one entry of a type's 'is' list, 'is Move if T is Move' (ir/types/struct.c)
    ErrorIsCondNoParms = 1276,  // A condition on an 'is' entry of a type with no type parameters, so nothing to vary by instance
    ErrorIsCondCopy = 1277,    // 'is Copy if ...': Copy is what a type is when nothing makes it move, so the condition is written on Move
    ErrorUnbuiltIsCond = 1278,  // A condition on an 'is' entry of a trait or an enum's variant, which is not built

    ErrorAwaitLeftCall = 1279,  // A borrow, or a place's base, written to the left of an 'await' in its statement and made by a call: it would be used after the seam, and only a plain path (a local, 'self', a field) is reached again there

    // 1280 was ErrorAwaitUnused; an awaited operation's unused result is warned, WarnAwaitUnused

    // An actor's 'init' runs on the state in the actor's block (parser/parseactor.c)
    ErrorActorStateInit = 1290, // An actor's state constructed anywhere but the making of the actor (its handle's initializer, and its init's '*self = new Self(...)'), by its fields' names or an 'init': a state in no actor, whose sends reach nothing

    // Futures: a behaviour's value stored and awaited later (actors.Future[T])
    ErrorAwaitNotFuture = 1281, // An 'await' on a value that is no future, behaviour's reply or operation: it waits for no answer
    ErrorFutureVoid = 1282,     // The value of a call of a behaviour returning nothing is used: there is no future to keep

    // 'Sized' and 'DynSized', granted by the compiler from a type's size (ir/types/struct.c)
    ErrorSizeMarkerUse = 1296,  // 'Sized' or 'DynSized' declared by a type: both are granted, by whether the type's size is known at compile time or carried by a reference to it, and a declaration could only contradict that

    // 'Immutable', declared by a type such as 'str' (ir/types/reference.c)
    ErrorImmutableWrite = 1297, // A permission that writes through a shared path ('mut', 'mut1', a lock permission) on a reference to a type declaring Immutable: nothing changes it through a reference

    // Hashing: core's Hash trait, and the hash the compiler supplies for a struct declaring it
    ErrorHashFloat = 1330,      // A struct declaring Hash with no hash of its own has a float field: NaN is not equal to itself and -0 is equal to 0, so no hash of the bits agrees with ==
    ErrorHashField = 1331,      // A struct declaring Hash with no hash of its own has a field whose type is not Hash, so the compiler has nothing to feed for it
    ErrorHashNoEq = 1332,       // A type declaring Hash has no '==': a hash is only meaningful against the equality that decides which keys are the same

    // Lending a body: a type body's 'use str via view'
    ErrorLend = 1333,           // A 'use T via m' that cannot lend: m is no method the type declares, T is an enum or a trait or the type itself, or it folds a member of T that is not a method

    // Characters
    ErrorCharNotNbr = 1334,     // A char where a number is wanted, or a number where a char is wanted: a char converts only explicitly, 'u8.from(c)' and 'char.from(b)'; a non-ASCII character literal is no u8

    // The C boundary
    ErrorCPtrConv = 1335,       // A slice or text where a raw pointer is wanted, or a raw pointer or text where a cstr is wanted: a slice's pointer is asked for with 'as', text reaches C as a cstr, and a pointer becomes a cstr with 'cstr.fromPtr'

    // Slices
    ErrorSliceSpelling = 1336,  // The retired spelling of a slice type, '&[]T': a slice is the borrow of the body of a run-time length, '&Array[T]' ('&[]mut T' is '&mut Array[T]', '&[]'a T' is '&'a Array[T]')

    // Literals
    ErrorLitBorrowWrite = 1337, // A string literal where a borrow that writes ('&mut T') is wanted, of a type declaring 'fromLiteral': a literal is lent as a temporary only to a read-only borrow, since a write to it is lost

    // Shape-changing types (ir/shapeinfer.c, ir/reshape.c)
    ErrorShapeMark = 1338,      // A type declaring 'ShapeChanging' that the compiler does not find so from its methods: none writes storage the type owns and lends a borrow. A declaration may only assert what is found
    ErrorShapeReshape = 1339,   // A call that could change the shape of a shape-changing value (a list, a string) while a borrow into it, reached through a path other references share, is still to be used: the change could come through another name for the same value

    // 'each' over cursors (ir/exp/each.c)
    ErrorEachItem = 1340,       // What an 'each' takes out of its cursor cannot be given to its variables: a 'next' that does not answer an Option, an 'iter' giving something with no 'next', variables that do not unpack the tuple item, or an item that moves

    // Generators (ir/exp/yield.c, parser/parsegen.c)
    ErrorYieldPlace = 1350,     // 'yield' outside the body of a function declared with 'yields'
    ErrorYieldReturn = 1351,    // A 'return' with a value in a generator, which gives its caller values only by 'yield'
    ErrorGenForm = 1352,        // A generator of a form not built: generic, a method, with 'inline' or a 'where' clause
    ErrorGenFrame = 1353,       // A local a 'yield' would keep in the generator's frame that the frame cannot hold: a lock's guard, or a value holding a traced reference

    // Loop control: a header 'if', a trailing 'if', a loop's 'else' (parser/parsefnflow.c)
    ErrorLoopElse = 1360,       // An 'else' on a loop that cannot run out: a 'while' with no condition
    ErrorTrailingIf = 1361,     // A trailing 'if' after a statement that takes none: only 'break', 'continue' and 'return' end with one

    // 'each' inside '<-' (parser/parseexpr.c, ir/exp/contents.c)
    ErrorEachEntry = 1370,      // An 'each' entry of '<-' written wrongly: with variables and no 'yield', a body in braces, or a 'yield' of nothing
    ErrorDrainItem = 1371,      // 'xs <- each src' over a source whose items come borrowed and move: a borrow cannot be copied out of
    ErrorEachMutSource = 1372,  // An 'each' (or 'parallel each') over a source written '&mut src': 'each' reads, and the items are changed through 'src.mutItems()'

    // 'parallel each' (ir/exp/pareach.c)
    ErrorParSource = 1380,      // A 'parallel each' over a source that cannot be split into independent pieces and report its size (a cursor, a generator, a file, a channel), or whose pieces the compiler cannot yet walk
    ErrorParControl = 1381,     // A 'break', 'return' or 'await', or a 'continue' of an outer loop, in a 'parallel each' body
    ErrorParWrite = 1382,       // A 'parallel each' body writes something declared outside the loop (or lends it for writing), which its pieces would do at the same time
    ErrorParRuntime = 1383,     // A 'parallel each' in a module that does not import the actors package its pieces run on, or in a generator (not built)
    ErrorParElse = 1384,        // A 'parallel each' with an 'else': its passes run at the same time and give no value
    ErrorParCopy = 1385,        // A 'parallel each' body copies a value that is not the pass's own and holds a counted owner whose count is not atomic (an Rc): the copy would write a count the passes share
    ErrorParBuilder = 1386,     // A parallel builder, 'xs <- parallel each ... yield v', whose receiver is not a List (a dictionary is built from a list of pairs after the loop)
    ErrorParFrame = 1388,       // A 'parallel each' in an actor's behaviour, which the behaviour is cut at, walks or reads a borrow or an array held in the behaviour's own frame, which is gone while the pieces run

    // The stopgap until the collector is per actor (ir/exp/pareach.c, ir/types/actor.c)
    ErrorGcStopgap = 1389,      // A 'Gc' (a value holding a traced reference) made or held inside an actor (state field, parameter, allocation, call, literal) or a 'parallel each' (body, header, filter, yield)

    // What a 'parallel each' body may reach (ir/exp/pareach.c)
    ErrorParReach = 1390,       // A 'parallel each' body, header, filter or yield names a value declared outside the loop (or its item) whose type is not safe to share across the passes: it holds, through a field, element, borrow or pointee, an aliasable owner of a region not declaring ThreadSafe (an Rc) or a traced reference

    // Closures (ir/exp/closure.c)
    ErrorClosureParm = 1391,    // A closure parameter written without a type where no signature is expected, or a signature the closure's parameters do not fit
    ErrorClosureRet = 1392,     // A closure whose paths give different types, and whose return type is not written
    ErrorClosureSelf = 1393,    // A closure that names itself: it cannot call itself
    ErrorClosureForm = 1394,    // A closure written where it cannot be built, or in a form it does not take
    ErrorClosureOverload = 1395, // A closure literal meeting overloaded callees it cannot choose between

    // An outside reference that could point at what a 'parallel each' writes (ir/exp/pareach.c)
    ErrorParAlias = 1396,       // A 'parallel each' that writes through its source (mutChunks, mutItems, a zip of them, a '&mut' slice) reads, by a path or as a whole, an outside variable that could reach a written element behind a reference: a read while a pass writes would race

    // Callable references (ir/types/fnsig.c, ir/exp/fncall.c)
    ErrorCallablePerm = 1397,   // A callable given where a '&<fn(sig)' or 'So[fn(sig)]' is wanted, refused because of the permission its '()' takes or the borrow lent
    ErrorCallableUse = 1398,    // A value made into a 'So[fn(sig)]' or 'Rc[fn(sig)]' that has no pub '()' of that signature
    ErrorClosureTrait = 1399,   // A closure literal given where a trait is wanted that does not have exactly one required method (and no field) for it to fill

    // What a GPU has none of, refused where it is written (ir/exp/borrow.c, ir/exp/allocate.c, ir/iexp.c, ir/types/reference.c, ir/exp/fncall.c, ir/exp/pareach.c, ir/exp/await.c, parser/parseactor.c)
    ErrorGpuUnavailable = 1400, // In GPU code, what a GPU has none of: a function reference ('&name'), a virtual reference made from a reference, an allocation ('new So[T]', 'new Rc[T]', ...), an owning reference type written ('So[T]', 'Rc[T]'), an actor, a 'parallel each' or an 'await' (threads), or a call of a C library function that is not math (I/O, the allocator): a GPU has no pointers to code, no tables of them, no allocator, no threads and no operating system

    // A virtual reference's trait method met by a stronger 'self' (ir/types/fnsig.c)
    ErrorVtableSelf = 1401,     // A struct viewed as a trait behind a virtual reference (`&<Trait`, `So[Trait]`) whose method takes a stronger `self` permission than the trait's method declares

    // Value parameters of a generic, '[N usize]' (ir/meta/generic.c, ir/types/array.c)
    ErrorGenValueParm = 1402,   // A value parameter, '[N usize]', that is not built: its annotation is not an integer type, it has another annotation beside it, it is on a generic type, or a 'where' clause asks about it
    ErrorGenValueArg = 1403,    // An argument for a value parameter that is not a non-negative integer literal of the parameter's type that fits it
    ErrorGenValueClash = 1404,  // A value parameter inferred as two different numbers: 'dot(&three, &four)' over 'Array[f32, N]' twice
    ErrorGenValueArith = 1405,  // Arithmetic over a value parameter in a type, 'Array[T, N + 1]': a size is written as a number or as a value parameter alone

    // The share check: the built-in marker 'Shareable' and markers after a '+' in a reference type (ir/itype.c, ir/meta/generic.c, ir/types/reference.c)
    ErrorNotShareable = 1406,   // An instance whose 'T is Shareable' is unmet, or a value made a reference marked '+ Shareable' (or '+ Sendable') that is not: the type holds a counted owner whose count is not atomic, a traced reference, or a reference that writes through a shared path ('&mut')
    ErrorMarkUse = 1407,        // A '+' in a reference type followed by something but Sendable or Shareable, or marking a reference that is not virtual ('&<Trait + Shareable', 'So[fn() + Sendable]')

    // A string literal where a cstr is wanted (ir/exp/literal.c)
    ErrorCStrNul = 1408,        // A string literal holding a NUL byte ('"ab\x00cd"') where a cstr is wanted: C would read it as 'ab', cutting the text short

    // Variants declared as a type (parser/parsetype.c, exp/cast.c)
    ErrorTypeVariant = 1409,    // A variant declared as a type, 'Ok i32;', in an enum that has common fields (every variant of such an enum names what it holds: write it as a struct), or bound through a managed reference, whose contents cannot be lent

    // Warnings
    WarnCode = 3000,
    WarnName = 3001,        // Unnecessary name
    // 3002 was WarnIndent, mixed tabs and spaces; indentation means nothing to the compiler
    WarnCopy = 3003,       // Unsafe attempt to copy a CopyMethod or CopyMove typed value
    WarnLoop = 3004,       // Infinite loop with no break
    WarnAwaitUnused = 3005, // The result an awaited operation (actors.Awaitable: an I/O operation) answers with, a failure among what it may say, is thrown away

    // Uncounted
    Uncounted = 9000,
};

extern int errors;
extern int warnings;
// Non-zero while a closure's body is tried under a guess at its permissions:
// a diagnostic is counted and not printed
extern int errorSilent;

// Send an error message to stderr
void errorExit(int exitcode, const char *msg, ...);
void errorMsgNode(INode *node, int code, const char *msg, ...);
void errorMsgLex(int code, const char *msg, ...);
void errorMsgLexAfter(int code, const char *msg, ...);
void errorMsg(int code, const char *msg, ...);
void errorUnreachable(INode *node, const char *msg);
void errorSummary();

#endif
