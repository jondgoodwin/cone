/** The Cloning pass that performs a deep copy of all nodes under the target node

 * - Clone parameters (including Self) substitute their instantiated values
 * - Variables are hygienic and generally not shared between contexts
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef clone_h
#define clone_h

// Context used across the cloning pass
typedef struct CloneState {
    INode *instnode;
} CloneState;

// Perform a deep clone of specified node
INode *cloneNode(CloneState *fstate, INode **nodep);

#endif