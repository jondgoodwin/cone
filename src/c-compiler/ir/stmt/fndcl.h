/** Handling for function/method declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fndcl_h
#define fndcl_h

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct FnDclNode {
	INamedNodeHdr;				// 'vtype': type of this name's value
	INode *value;				// Block or intrinsic code nodes (NULL if no code)
	LLVMValueRef llvmvar;		// LLVM's handle for a declared variable (for generation)
    struct FnDclNode *nextnode;     // Link to next overloaded method with the same name (or NULL)
} FnDclNode;

FnDclNode *newFnDclNode(Name *namesym, uint16_t tag, INode *sig, INode *val);
void fnDclPrint(FnDclNode *fn);
void fnDclPass(PassState *pstate, FnDclNode *node);

#endif