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

// All types get a copy trait indicating how to handle when a value of that type
// is assigned to a variable or passed as an argument to a function.
// This is because a bitwise copy of certain values (e.g., pointers, unique reference,
// or destructible resources) can be potentially unsafe.
enum CopyTrait {
    CopyBitwise,   // A value can be safely copied using bitwise memcpy
    CopyMethod,    // A value can be safely copied using the type's .copy method (semantic copy)
    CopyMove       // A value can only be moved (bitwise copy and then deactivate the source)
};

// Return node's type's declaration node
// (Note: only use after it has been type-checked)
INode *itypeGetTypeDcl(INode *node);

// Return 1 if nominally (or structurally) identical, 0 otherwise.
// Nodes must both be types, but may be name use or declare nodes.
int itypeIsSame(INode *node1, INode *node2);

// Is totype equivalent or a non-coerced subtype of fromtype
// 0 - no
// 1 - yes, without conversion
// 2+ - requires increasingly lossy conversion/coercion
int itypeMatches(INode *totype, INode *fromtype);

// Return a CopyTrait indicating how to handle when a value is assigned to a variable or passed to a function.
int itypeCopyTrait(INode *typenode);

// Add type mangle info to buffer
char *itypeMangle(char *bufp, INode *vtype);

// Return true if type has a concrete and instantiable. 
// Opaque (field-less) structs, traits, functions, void will be false.
int itypeIsConcrete(INode *type);

#endif