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
    INode *instnode;     // The node provoking instantiation (for error messages)
    INode *selftype;     // Self type (might be NULL)
    uint16_t scope;      // Current block level
} CloneState;

// Data structures used for fixing dcl pointers
typedef struct CloneDclMap {
    INode *original;
    INode *clone;
} CloneDclMap;

// Perform a deep clone of specified node
INode *cloneNode(CloneState *fstate, INode *nodep);

// Instantiate a clone of some tree
INode *cloneTree(INode *tree, INode *instnode, INode *selftype, uint32_t scope);

// Instantiate a clone of some tree, substituting args for parameters
INode *cloneTreeParms(INode *tree, Nodes *parms, Nodes *args, INode *instnode, INode *selftype, uint32_t scope);

// Preserve high-water position in the dcl stack
uint32_t cloneDclPush();

// Restore high-water position in the dcl stack
void cloneDclPop(uint32_t pos);

// Remember a mapping of a declaration node between the original and a copy
void cloneDclSetMap(INode *orig, INode *clone);

// Return the new pointer to the dcl node, given the original pointer
INode *cloneDclFix(INode *orig);

#endif