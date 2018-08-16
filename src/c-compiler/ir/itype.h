/** Compiler types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef type_h
#define type_h

#include <stdint.h>
typedef struct Name Name;

// Named type node header (most types are named)
#define INamedTypeNodeHdr \
    INamedNodeHdr; \
    LLVMTypeRef llvmtype

// Named type node interface (most types are named)
// A named type needs to remember generated LLVM type ref for typenameuse nodes
typedef struct NamedTypeNode {
    INamedTypeNodeHdr;
} NamedTypeNode;

// All types get a copy trait indicating how to handle when a value of that type
// is assigned to a variable or passed as an argument to a function.
// This is because a bitwise copy of certain values (e.g., pointers, unique reference,
// or destructible resources) can be potentially unsafe.
enum CopyTrait {
    CopyBitwise,   // A value can be safely copied using bitwise memcpy
    CopyMethod,    // A value can be safely copied using the type's .copy method (semantic copy)
    CopyMove       // A value can only be moved (bitwise copy and then deactivate the source)
};

// Void type - e.g., for fn with no return value
typedef struct VoidTypeNode {
	INodeHdr;
} VoidTypeNode;

INode *typeGetVtype(INode *node);
INode *typeGetDerefType(INode *node);
int typeIsSame(INode *node1, INode *node2);
int typeMatches(INode *totype, INode *fromtype);
int typeCoerces(INode *to, INode **from);
int typeCopyTrait(INode *typenode);
void typeHandleCopy(INode **nodep);


char *typeMangle(char *bufp, INode *vtype);

VoidTypeNode *newVoidNode();
void voidPrint(VoidTypeNode *voidnode);

#endif