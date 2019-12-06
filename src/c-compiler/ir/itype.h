/** Generic Type node handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef itype_h
#define itype_h

typedef struct Name Name;

// Named type node header (most types are named)
#define ITypeNodeHdr \
    INodeHdr; \
    LLVMTypeRef llvmtype

// Named type node interface (most types are named)
// A named type needs to remember generated LLVM type ref for typenameuse nodes
typedef struct ITypeNode {
    ITypeNodeHdr;
} ITypeNode;

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *itypeGetTypeDcl(INode *node);

// Return node's type's declaration node (or pvtype if a ref or ptr)
INode *itypeGetDerefTypeDcl(INode *node);

// Type check node, expecting it to be a type. Give error and return 0, if not.
int itypeTypeCheck(TypeCheckState *pstate, INode **node);

// Return 1 if nominally (or structurally) identical, 0 otherwise.
// Nodes must both be types, but may be name use or declare nodes.
int itypeIsSame(INode *node1, INode *node2);

// Is totype equivalent or a non-coerced subtype of fromtype
// 0 - no
// 1 - yes, without conversion
// 2+ - requires increasingly lossy conversion/coercion
int itypeMatches(INode *totype, INode *fromtype);

// Add type mangle info to buffer
char *itypeMangle(char *bufp, INode *vtype);

// Return true if type has a concrete and instantiable. 
// Opaque (field-less) structs, traits, functions, void will be false.
int itypeIsConcrete(INode *type);

#endif