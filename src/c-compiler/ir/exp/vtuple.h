/** Handling for value tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef vtuple_h
#define vtuple_h

// Create a new value tuple node
TupleNode *newVTupleNode();

// Clone value tuple
INode *cloneVTupleNode(CloneState *cstate, TupleNode *node);

// Serialize a value tuple node
void vtuplePrint(TupleNode *tuple);

// Name resolution for vtuple
void vtupleNameRes(NameResState *pstate, TupleNode *tuple);

// Type check the value tuple node
// - Infer type tuple from types of vtuple's values
void vtupleTypeCheck(TypeCheckState *pstate, TupleNode *node);

#endif