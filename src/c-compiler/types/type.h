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
#define NamedTypeAstHdr \
    NamedAstHdr; \
    LLVMTypeRef llvmtype

// Named type node interface (most types are named)
// A named type needs to remember generated LLVM type ref for typenameuse nodes
typedef struct NamedTypeAstNode {
    NamedTypeAstHdr;
} NamedTypeAstNode;

// Named type that supports methods
#define MethodTypeAstHdr \
    NamedTypeAstHdr; \
	Nodes *methods; \
	Nodes *subtypes

// Interface for a named type that supports methods
// -> methods (Nodes) is the dictionary of named methods
// -> subtypes (Nodes) is the list of trait/interface subtypes it implements
typedef struct MethodTypeAstNode {
    MethodTypeAstHdr;
} MethodTypeAstNode;

// Type Ast Node header for all type structures
// - methods is the list of a type instance's methods
// - subtypes is the list of traits, etc. the type implements
#define TypeAstHdr \
	TypedAstHdr; \
	Nodes *methods; \
	Nodes *subtypes

// Castable structure for all type AST nodes
typedef struct TypeAstNode {
    TypeAstHdr;
} TypeAstNode;

// Named type declaration node
typedef struct TypeDclAstNode {
    NamedAstHdr;				// 'vtype': type of this name's value
    LLVMTypeRef llvmtype;		// LLVM's handle for a declared type (for generation)
    AstNode *typedefnode;				// Type's declaration node (NULL if not initialized)
} TypeDclAstNode;

// Void type - e.g., for fn with no return value
typedef struct VoidTypeAstNode {
	BasicAstHdr;
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