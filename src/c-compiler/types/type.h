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

// Named type declaration node
typedef struct TypeDclAstNode {
    NamedAstHdr;				// 'vtype': type of this name's value
    LLVMTypeRef llvmtype;		// LLVM's handle for a declared type (for generation)
    AstNode *value;				// Type's declaration node (NULL if not initialized)
} TypeDclAstNode;

// Void type - e.g., for fn with no return value
typedef struct VoidTypeAstNode {
	TypeAstHdr;
} VoidTypeAstNode;

// For arrays
typedef struct ArrTypeAstNode {
	TypeAstHdr;
	uint32_t nbrelems;		// Number of elements
	AstNode *elemtype;	// Type of array's elements
} ArrTypeAstNode;

AstNode *typeGetVtype(AstNode *node);
int typeIsSame(AstNode *node1, AstNode *node2);
int typeMatches(AstNode *totype, AstNode *fromtype);
int typeCoerces(AstNode *to, AstNode **from);

char *typeMangle(char *bufp, AstNode *vtype);

VoidTypeAstNode *newVoidNode();
void voidPrint(VoidTypeAstNode *voidnode);

TypeDclAstNode *newTypeDclNode(Name *namesym, uint16_t asttype, AstNode *type, AstNode *val);
void newTypeDclNodeStr(char *namestr, uint16_t asttype, AstNode *type);
void nameVtypeDclPass(PassState *pstate, TypeDclAstNode *name);
void typeDclPrint(TypeDclAstNode *name);

#endif