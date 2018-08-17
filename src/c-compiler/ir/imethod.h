/** Shared logic for method-based types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef imethod_h
#define imethod_h

// Variable-sized structure holding an ordered list of Nodes
// These nodes are methods (potentially overloaded) or properties
typedef struct IMethNodes {
	uint32_t used;
	uint32_t avail;
    INode **nodes;
} IMethNodes;

// Named type that supports methods, properties and traits
#define IMethodNodeHdr \
    INamedTypeNodeHdr; \
    IMethNodes methprops; \
	Nodes *subtypes

// Interface for a named type that supports methods
// -> methprops (IMethNodes) is the dictionary of named methods
// -> subtypes (Nodes) is the list of trait/interface subtypes it implements
typedef struct IMethodNode {
    IMethodNodeHdr;
} IMethodNode;

// Needed for helper functions
typedef struct FnDclNode FnDclNode;
typedef struct VarDclNode VarDclNode;

// Initialize methnodes metadata in a method type node
void imethnodesInit(IMethNodes *mnodes, uint32_t size);

// Iterate through the set of IMethNodes 
#define imethnodesFor(mnodes, cnt, nodesp) nodesp = (mnodes)->nodes, cnt = (mnodes)->used; cnt; cnt--, nodesp++

// Point to the indexth node in an IMethNodes
#define imethnodesGet(mnodes, index) (mnodes)->nodes[index]

// Point to the last node in an IMethNodes
#define imethnodesLast(mnodes) imethnodesGet(mnodes, mnodes->used-1)

// Add an INode to the end of a IMethNodes, growing it if full (changing its memory location)
void imethnodesAdd(IMethNodes *mnodes, INode *node);

// Add a static function or potentially overloaded method
// If method is overloaded, add it to the link chain of same named methods
void imethnodesAddFn(IMethNodes *mnodes, FnDclNode *fnnode);

// Add a property node to a method type
void imethnodesAddProp(IMethNodes *mnodes, VarDclNode *fnnode);

// Find the desired named node (could be method or property)
// Return the node, if found or NULL if not found
INamedNode *imethnodesFind(IMethNodes *mnodes, Name *name);

// Find method that best fits the passed arguments
// 'firstmethod' is the first method that matches the name
// We follow its forward links to find one whose parameter types best match args types
FnDclNode *imethnodesFindBestMethod(FnDclNode *firstmethod, Nodes *args);

#endif