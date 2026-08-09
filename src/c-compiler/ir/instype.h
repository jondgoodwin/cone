/** Shared logic for namespace-based types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef instype_h
#define instype_h

// Namespaced type that supports named nodes (e.g., methods) and traits
#define INsTypeNodeHdr \
    ITypeNodeHdr; \
    NodeList nodelist; \
    Namespace namespace; \
    INode *dropfn

// Interface for a namespaced type
// -> nodes (NodeList) is the list of nodes
// -> namespace is the dictionary of names (methods, fields)
// -> subtypes (Nodes) is the list of trait/interface subtypes it implements
typedef struct INsTypeNode {
    INsTypeNodeHdr;
} INsTypeNode;

// Needed for helper functions
typedef struct FnDclNode FnDclNode;
typedef struct VarDclNode VarDclNode;

// Initialize common fields
void iNsTypeInit(INsTypeNode *type, int nodecnt);

// Add a static function or potentially overloaded method to dictionary
// The first method declared for a name binds directly to its FnDclNode.
// A second same-named method replaces that binding with an FnOverloadDclNode
// holding both, and later methods are appended to that node's candidates.
void iNsTypeAddFnDict(INsTypeNode *type, FnDclNode *fnnode);

// Add a function/method to type's dictionary and owned list
void iNsTypeAddFn(INsTypeNode *type, FnDclNode *fnnode);

// Find the named node (could be method or field)
// Return the node, if found or NULL if not found
INode *iNsTypeFindFnField(INsTypeNode *type, Name *name);

// Find method that best fits the passed arguments
// 'binding' is the namespace's binding for the method's name:
// either a single FnDclNode or an FnOverloadDclNode holding all candidates
FnDclNode *iNsTypeFindBestMethod(INode *binding, INode **self, Nodes *args);

// Find the first pointer/reference method candidate that accepts the passed arguments
// 'binding' is the namespace's binding for the method's name
FnDclNode *iNsTypeFindPtrMethod(INode *binding, Nodes *args);

// Find method whose method signature matches exactly (except for self)
// 'binding' is the namespace's binding for the method's name
// return NULL if none
FnDclNode *iNsTypeFindVrefMethod(INode *binding, FnDclNode *matchmeth);

#endif
