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
    ExitGen = 6,    // Unrecoverable failure during code generation

    // Non-terminating errors
    ErrorCode = 1000,
    ErrorBadTok = 1001,    // Bad character starting an unknown token
    ErrorGenErr = 1002,    // Failure to perform generation activity
    ErrorNoSemi = 1003,    // Missing semicolon
    ErrorNoLCurly = 1004,  // Missing left curly brace
    ErrorNoRCurly = 1005,    // Missing right curly brace
    ErrorBadTerm = 1006,   // Invalid term - something other than var, lit, etc.
    ErrorBadGloStmt = 1007, // Invalid global area type, var or function statement
    ErrorDupImpl = 1008,    // Function already has another implementation
    ErrorNoLParen = 1009,  // Expected left parenthesis not found
    ErrorNoRParen = 1010,    // Expected right parenthesis not found
    ErrorDupName = 1011,    // Duplicate name declaration
    ErrorNoName = 1012,    // Name is required but not provided
    ErrorInvType = 1013,    // Types do not match correctly
    ErrorNoIdent = 1014,    // Identifier expected but not provided
    ErrorNotLit = 1015,    // Value can only be a literal
    ErrorBadLval = 1016,    // Expression is not an lval
    ErrorNoMut = 1017,        // Mutation is not allowed
    ErrorNotFn = 1018,        // Not a function
    ErrorUnkName = 1019,    // Unknown name (no declaration exists)
    ErrorNoType = 1020,    // No type specified (or inferrable)
    ErrorNoInit = 1021,    // Parm didn't specify required default value
    ErrorFewArgs = 1022,    // Too few arguments specified
    ErrorManyArgs = 1023,    // Too many arguments specified
    ErrorNoMbr = 1024,        // Method/field not specified
    ErrorNoMeth = 1025,    // No such method defined by the object's type
    ErrorRetNotLast = 1026, // Return was found not at the end of the block
    ErrorNoRet = 1027,        // Return value expected but not given
    ErrorNoElse = 1028,    // Missing 'else' branch
    ErrorNoLoop = 1029,    // 'break' or 'continue' allowed only in while/each loop
    ErrorNoVtype = 1030,    // Missing value type
    ErrorNotPtr = 1031,    // Not a pointer
    ErrorNotLval = 1032,    // Not an lval
    ErrorAddr = 1033,        // Invalid expr to get an addr (&) of
    ErrorBadPerm = 1034,    // Permission not allowed
    ErrorNoFlds = 1035,    // Expression's type does not support fields
    ErrorBadAlloc = 1036,  // Missing valid alloc methods
    ErrorNoDbl = 1037,        // Missing '::'
    ErrorNoVar = 1038,        // Missing variable name
    ErrorNoEof = 1039,        // Missing end-of-file
    ErrorNoImpl = 1040,    // Function must be implemented
    ErrorBadImpl = 1041,    // Function must not be implemented
    ErrorNotPublic = 1042, // Method is not public
    ErrorBadMeth = 1043,   // Methods/fields not supported
    ErrorNotTyped = 1044,  // Expected a value that has a type
    ErrorBadIndex = 1045,  // Bad index/slice on array/ref
    ErrorBadArray = 1046,  // Bad array
    ErrorBadSlice = 1047,  // Bad slice type
    ErrorMove = 1048,      // Move error of some kind
    ErrorRecurse = 1049,   // Recursive type error
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
void errorSummary();

#endif
