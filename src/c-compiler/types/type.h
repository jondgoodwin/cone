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

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct TypeDclAstNode {
    NamedAstHdr;				// 'vtype': type of this name's value
    AstNode *value;				// Starting value/declaration (NULL if not initialized)
    LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
    uint16_t scope;				// 0=global
    uint16_t index;				// index within this scope (e.g., parameter number)
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