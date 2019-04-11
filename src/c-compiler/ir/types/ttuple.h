/** Handling for type tuple nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef ttuple_h
#define ttuple_h

// A type tuple is a comma-separated list of types, each different
// It acts like an ad hoc struct, briefly binding together types
// for a parallel assignment or multiple return values
typedef struct TTupleNode {
    ITypedNodeHdr;
    Nodes *types;
} TTupleNode;

// Create a new type tuple node
TTupleNode *newTTupleNode(int cnt);

// Serialize a type tuple node
void ttuplePrint(TTupleNode *tuple);

// Name resolution of type tuple node
void ttupleNameRes(PassState *pstate, TTupleNode *node);

// Type check type tuple node
void ttupleWalk(PassState *pstate, TTupleNode *node);

#endif