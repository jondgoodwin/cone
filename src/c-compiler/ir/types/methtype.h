/** Shared logic for method-based types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef methtype_h
#define methtype_h

typedef struct FnDclNode FnDclNode;
typedef struct VarDclNode VarDclNode;

// Metadata describing a variable-sized structure holding an ordered list of Nodes
// These nodes are methods (potentially overloaded) or properties
typedef struct MethNodes {
	uint32_t used;
	uint32_t avail;
    INode **nodes;
} MethNodes;

// Named type that supports methods (and properties)
#define IMethodNodeHdr \
    INamedTypeNodeHdr; \
    MethNodes methprops; \
	Nodes *subtypes

// Interface for a named type that supports methods
// -> methods (Nodes) is the dictionary of named methods
// -> subtypes (Nodes) is the list of trait/interface subtypes it implements
typedef struct MethodTypeNode {
    IMethodNodeHdr;
} MethodTypeNode;

// Helper Functions
void methnodesInit(MethNodes *mnodes, uint32_t size);
void methnodesAdd(MethNodes *mnodes, INode *node);
void methnodesAddFn(MethNodes *mnodes, FnDclNode *fnnode);
void methnodesAddProp(MethNodes *mnodes, VarDclNode *fnnode);
INamedNode *methnodesFind(MethNodes *mnodes, Name *name);

#define methnodesNodes(nodes) ((INode**)((nodes)+1))
#define methnodesFor(mnodes, cnt, nodesp) nodesp = (mnodes)->nodes, cnt = (mnodes)->used; cnt; cnt--, nodesp++
#define methnodesGet(mnodes, index) (mnodes)->nodes[index]
#define methnodesLast(mnodes) methnodesGet(mnodes, mnodes->used-1)

FnDclNode *methnodesFindBestMethod(FnDclNode *firstmethod, Nodes *args);

#endif