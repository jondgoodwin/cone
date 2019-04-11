/** Handling for function/method declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fndcl_h
#define fndcl_h

// Name declaration node (e.g., variable, fn implementation, or named type)
typedef struct FnDclNode {
    INamedNodeHdr;                // 'vtype': type of this name's value
    INode *value;                // Block or intrinsic code nodes (NULL if no code)
    LLVMValueRef llvmvar;        // LLVM's handle for a declared variable (for generation)
    struct FnDclNode *nextnode;     // Link to next overloaded method with the same name (or NULL)
} FnDclNode;

FnDclNode *newFnDclNode(Name *namesym, uint16_t tag, INode *sig, INode *val);
void fnDclPrint(FnDclNode *fn);

/// Resolve all names in a function
void fnDclNameRes(NameResState *pstate, FnDclNode *name);

// Type checking a function's logic, does more than you might think:
// - Turn implicit returns into explicit returns
// - Perform type checking for all statements
// - Perform data flow analysis on variables and references
void fnDclTypeCheck(TypeCheckState *pstate, FnDclNode *fnnode);

#endif