/** Handling for const declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef const_h
#define const_h

// Variable declaration node (global, local, parm)
typedef struct ConstDclNode {
    IExpNodeHdr;             // 'vtype': type of this name's value
    Name *namesym;
    INode *value;              // Starting value/declaration (NULL if not initialized)
} ConstDclNode;

ConstDclNode *newConstDclNode(Name *namesym);

// Create a new variable dcl node that is a copy of an existing one
INode *cloneConstDclNode(CloneState *cstate, ConstDclNode *node);

void constDclPrint(ConstDclNode *fn);

// Name resolution of vardcl
void constDclNameRes(NameResState *pstate, ConstDclNode *node);

// Type check vardcl
void constDclTypeCheck(TypeCheckState *pstate, ConstDclNode *node);

#endif
