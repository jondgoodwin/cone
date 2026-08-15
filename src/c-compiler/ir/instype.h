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

// Outcome of testing every candidate a name declares against one call
enum OverloadMatch {
    OverloadNone,       // No candidate accepts the call's arguments
    OverloadUnique,     // Exactly one candidate accepts the call's arguments
    OverloadAmbiguous   // More than one candidate accepts the call's arguments
};

// Initialize common fields
void iNsTypeInit(INsTypeNode *type, int nodecnt);

// Bind a static function or method to its unique concrete name and, when it
// declares an overload name, to that name's separate FnOverloadDclNode
void iNsTypeAddFnDict(INsTypeNode *type, FnDclNode *fnnode);

// Add a function/method to type's dictionary and owned list
void iNsTypeAddFn(INsTypeNode *type, FnDclNode *fnnode);

// Find the named node (could be method or field)
// Return the node, if found or NULL if not found
INode *iNsTypeFindFnField(INsTypeNode *type, Name *name);

// Find the one method candidate that accepts the call's receiver and arguments.
// 'binding' is the namespace's binding for the name the caller used: either a
// single concrete FnDclNode or an FnOverloadDclNode holding all candidates.
// Every candidate is tested without altering the call. '*status' says whether no
// candidate, exactly one candidate, or more than one candidate accepted it.
FnDclNode *iNsTypeFindMethod(INode *binding, INode **self, Nodes *args, enum OverloadMatch *status);

// Find the one pointer/reference method candidate that accepts the passed arguments.
// Every candidate is tested without altering the call, and pointer/reference
// parameters must be the same type as self rather than merely coercible.
FnDclNode *iNsTypeFindPtrMethod(INode *binding, Nodes *args, enum OverloadMatch *status);

// Find method whose method signature matches exactly (except for self)
// 'binding' is the namespace's binding for the method's name
// return NULL if none, or if more than one candidate matches
FnDclNode *iNsTypeFindVrefMethod(INode *binding, FnDclNode *matchmeth);

#endif
