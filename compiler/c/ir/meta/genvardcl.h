/** Handling for generic variable declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef genvardcl_h
#define genvardcl_h

// Generic variable declaration node
typedef struct GenVarDclNode {
    IExpNodeHdr;             // 'vtype': type of this name's value
    Name *namesym;
    // What is written after the parameter's name, '[T Integer + Copy]': each
    // '+'-joined name, or NULL. Its meaning is what it resolves to: a trait
    // makes it a constraint (genericConstraintsNameRes); a type or a kind, a
    // value or kind parameter. A kind parameter is not built.
    Nodes *annot;
    // The type of a value parameter: one slot, two meanings by declaration.
    // On a generic, '[N: usize]': its annotation named an integer type, which
    // genericConstraintsNameRes takes out of the constraints and records here,
    // and an argument is a compile-time literal of it (genericValueArg). On a
    // macro, '[a: T]', '[a: lazy T]': the type written after the ':', which the
    // parser records here (FlagLazy for 'lazy'), and an argument is a run-time
    // value of it. NULL for a type parameter, and for a macro method's 'self'.
    INode *valtype;
} GenVarDclNode;

// Create a new generic variable declaraction node
GenVarDclNode *newGVarDclNode(Name *namesym);

void gVarDclPrint(GenVarDclNode *var);

// Name resolution
void gVarDclNameRes(NameResState *pstate, GenVarDclNode *var);

// Type check generic variable declaration
void gVarDclTypeCheck(TypeCheckState *pstate, GenVarDclNode *var);

#endif
