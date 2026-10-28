/** Handling for generic nodes (also used for macros)
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef macro_h
#define macro_h

// Macro declaration node
typedef struct MacroDclNode {
    IExpNodeHdr;             // 'vtype': type of this name's value
    Name *namesym;
    // The type parameters, 'macro m[T, U][a: T, b: U]': the first of two bracket
    // lists, NULL when only one is written. A use gives their arguments in its
    // brackets, 'm[i32, f64](1, 2.)'; inferring them is not built.
    Nodes *typeparms;
    // The value parameters (GenVarDcl), each typed: 'a: T', 'a: lazy T' (its
    // 'valtype' and FlagLazy), except a macro method's 'self', which is bare
    Nodes *parms;
    INode *body;             // The body of the generic
    Nodes *memonodes;        // Pairs of memoized generic calls and cloned bodies
} MacroDclNode;

// Create a new macro declaraction node
MacroDclNode *newMacroDclNode(Name *namesym);

// Deep copy a macro declaration, for a generic type's instance
INode *cloneMacroDclNode(CloneState *cstate, MacroDclNode *node);

void macroPrint(MacroDclNode *fn);

// Name resolution
void macroNameRes(NameResState *pstate, MacroDclNode *node);

// Type check generic
void macroTypeCheck(TypeCheckState *pstate, MacroDclNode *node);

// Expand a macro named where a value is expected
void macroNameTypeCheck(TypeCheckState *pstate, NameUseNode **macro);

// Expand a macro called by name, substituting the arguments for its parameters
void macroCallTypeCheck(TypeCheckState *pstate, FnCallNode **nodep);

// Expand a macro called with its type arguments in brackets and its value
// arguments in parentheses, 'm[i32](x)': the call's callee is the bracketed use
void macroCallTypeArgsTypeCheck(TypeCheckState *pstate, FnCallNode **nodep);

// Expand a macro method called on a receiver, which stands in for 'self'.
// 'typeargs' are what brackets after its name gave its type parameters, or NULL.
void macroMethodTypeCheck(TypeCheckState *pstate, FnCallNode **nodep, MacroDclNode *macro, Nodes *typeargs);

// Refuse, at 'at', a change to the variable a macro's expansion bound a plain
// parameter to (FlagMacroBind): an argument the macro assigns to must be 'lazy'
void macroParmAssignRefused(INode *at, VarDclNode *var);

#endif
