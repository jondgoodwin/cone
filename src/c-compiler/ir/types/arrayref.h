/** Handling for array reference (slice) type
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef arrayref_h
#define arrayref_h

// Serialize an array reference type
void arrayRefPrint(RefNode *node);

// Name resolution of an array reference node
void arrayRefNameRes(PassState *pstate, RefNode *node);

// Semantically analyze an array reference node
void arrayRefPass(PassState *pstate, RefNode *name);

// Compare two reference signatures to see if they are equivalent
int arrayRefEqual(RefNode *node1, RefNode *node2);

// Will from reference coerce to a to reference (we know they are not the same)
int arrayRefMatches(RefNode *to, RefNode *from);

#endif