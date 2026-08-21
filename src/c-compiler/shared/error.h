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
    ExitIndent = 5,    // Too many indent levels in lexer
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
    ErrorBadAlloc = 1036,  // Region cannot allocate: missing or ill-formed _alloc method
    // 1037 was ErrorNoDbl; no construct requires a '::' that could be missing
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

    // Array literals
    ErrorBadFill = 1059,        // Array fill literal may not repeat this value
    ErrorFillCount = 1060,      // Array fill literal's element count cannot be counted into

    // Generics
    ErrorNoGenParms = 1061,     // Type parameter list declares no parameters

    // Iteration
    ErrorNotIterable = 1062,    // Value cannot be iterated over by 'each'

    // IR well-formedness (--checktree). A compiler defect, not a bad program
    ErrorBadTree = 1063,        // A node was left with no type or no body

    // Lifetimes
    ErrorEscape = 1064,         // A borrowed reference would outlive what it borrows from

    // Words held for language features not yet implemented
    ErrorReserved = 1065,       // Reserved word used as an identifier

    // Reinterpretation, checked where the size is known
    ErrorRecastSize = 1066,     // 'as' onto a struct whose size differs from the source's

    // Generic and macro expansion
    ErrorInstDepth = 1067,      // Expansion nested deeper than the compiler will follow

    // Demand-driven analysis
    ErrorCircular = 1068,       // A declaration whose type comes from a value that names it back

    // Layout
    ErrorNoSize = 1069,         // A value whose type cannot report a size

    // Argument lists, split out of ErrorManyArgs, which now means only that a
    // call passed more arguments than the declaration accepts
    ErrorArgCount = 1070,       // An instantiation's argument count is not its parameter count
    ErrorNotType = 1071,        // A generic argument that must be a type is not one
    ErrorNoArgs = 1072,         // A generic or macro with parameters was named without arguments
    ErrorFldArgs = 1073,        // Arguments given to a field access, which accepts none

    // Reference types
    ErrorNoRefType = 1074,      // A reference type did not name what it refers to

    // The compiler's own invariants. This is the one code no source is supposed
    // to be able to produce, and so the one code with no scenario: reaching it
    // means a compiler defect, not a bad program. See errorUnreachable.
    ErrorUnreachable = 1075,    // A state the compiler had established cannot happen

    // Warnings
    WarnCode = 3000,
    WarnName = 3001,        // Unnecessary name
    WarnIndent = 3002,        // Inconsistent indent character
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
void errorMsg(int code, const char *msg, ...);
void errorUnreachable(INode *node, const char *msg);
void errorSummary();

#endif
