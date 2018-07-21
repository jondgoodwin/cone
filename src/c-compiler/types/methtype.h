/** Shared logic for method-based types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef methtype_h
#define methtype_h

// Metadata describing a variable-sized structure holding an ordered list of AstNodes
// These nodes are methods (potentially overloaded) or fields
typedef struct MethNodes {
	uint32_t used;
	uint32_t avail;
    AstNode **nodes;
} MethNodes;

// Named type that supports methods
#define MethodTypeAstHdr \
    NamedTypeAstHdr; \
    Namespace methfields; \
	Nodes *subtypes

// Interface for a named type that supports methods
// -> methods (Nodes) is the dictionary of named methods
// -> subtypes (Nodes) is the list of trait/interface subtypes it implements
typedef struct MethodTypeAstNode {
    MethodTypeAstHdr;
} MethodTypeAstNode;

// Helper Functions
void methnodesInit(MethNodes *mnodes, uint32_t size);
void methnodesAdd(MethNodes *mnodes, AstNode *node);
NamedAstNode *methnodesFind(MethNodes *mnodes, Name *name);

#define methnodesNodes(nodes) ((AstNode**)((nodes)+1))
#define methnodesFor(mnodes, cnt, nodesp) nodesp = (mnodes)->nodes, cnt = (mnodes)->used; cnt; cnt--, nodesp++
#define methnodesGet(mnodes, index) (mnodes)->nodes[index]
#define methnodesLast(mnodes) methnodesGet(mnodes, mnodes->used-1)

#endif