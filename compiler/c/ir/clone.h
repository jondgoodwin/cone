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
    INode *selfparm;     // Expanding a macro method: the parameter standing for its receiver, else NULL
    INode *srcsite;      // Expanding a macro: its outermost use, where the body's 'srcFile()'
                         // and 'srcLine()' are placed (cloneFnCallNode), else NULL
    INode *structshell;  // An instance of a generic type reserved in advance, which the struct cloned next fills
    Nodes *absent;       // That instance's members whose 'where' its arguments do not meet, which it is cloned without
    Nodes *late;         // That instance's members whose 'where' waits on an argument's layout: cloned, and settled once layouts are done (genericLateWhere)
    Nodes *latepairs;    // Out: each of 'late' beside the copy made of it, alternating, the original first
    uint16_t scope;     // Current block level
    uint16_t modinst;   // Cloning a generic module's instance (modInstantiate): a generic type it declares is copied still generic, and a type naming a trait it declares names the copy (cloneStructNode)
    Nodes *enrichfix;   // Out, with modinst: each alias an enrichment's copy re-binds, beside the copy of its base, alternating (structCloneEnrichFix)
} CloneState;

// Data structures used for fixing dcl pointers
typedef struct CloneDclMap {
    INode *original;
    INode *clone;
} CloneDclMap;

// Perform a deep clone of specified node
INode *cloneNode(CloneState *cstate, INode *nodep);

// Set the state needed for deep cloning some node
void clonePushState(CloneState *cstate, INode *instnode, INode *selftype, uint32_t scope, Nodes *parms, Nodes *args);

// Release the acquired state
void clonePopState();

// Preserve high-water position in the dcl stack
uint32_t cloneDclPush();

// Restore high-water position in the dcl stack
void cloneDclPop(uint32_t pos);

// Remember a mapping of a declaration node between the original and a copy
void cloneDclSetMap(INode *orig, INode *clone);

// Return the new pointer to the dcl node, given the original pointer
INode *cloneDclFix(INode *orig);

#endif
