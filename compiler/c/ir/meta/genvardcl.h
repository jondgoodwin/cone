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
    // value or kind parameter, which are not built.
    Nodes *annot;
} GenVarDclNode;

// Create a new generic variable declaraction node
GenVarDclNode *newGVarDclNode(Name *namesym);

void gVarDclPrint(GenVarDclNode *var);

// Name resolution
void gVarDclNameRes(NameResState *pstate, GenVarDclNode *var);

// Type check generic variable declaration
void gVarDclTypeCheck(TypeCheckState *pstate, GenVarDclNode *var);

#endif
