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
    // The integer type of a value parameter, '[N usize]': its annotation named
    // a number type, which genericConstraintsNameRes takes out of the
    // constraints and records here. NULL for a type parameter. An argument is
    // a literal of this type (genericValueArg).
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
