/** Handling for function/method declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fndcl_h
#define fndcl_h

// Function/method declaration node
typedef struct FnDclNode {
    IExpNodeHdr;                // 'vtype': type of this name's value
    Name *namesym;
    Name *overloadsym;            // Overload name this declaration also joins (NULL if none)
    INode *value;                 // Block or intrinsic code nodes (NULL if no code)
    LLVMValueRef llvmvar;         // LLVM's handle for a declared variable (for generation)
    char *genname;                // Name of the function as known to the linker
    GenericInfo *genericinfo;     // Link to generic parms, etc (or NULL if not generic)
    uint16_t vtblidx;             // Method ptr's index in the type's vtable
} FnDclNode;

// Overloaded function/method declaration node.
// It is the namespace binding for an explicitly declared overload name.
// It has no type, value, or generated symbol: every executable implementation
// remains a separate FnDclNode found in 'overloads', bound to its own concrete name.
typedef struct FnOverloadDclNode {
    INodeHdr;
    Name *namesym;
    Nodes *overloads;             // Ordered list of FnDclNode candidates
} FnOverloadDclNode;

// Create a new function declaraction node
FnDclNode *newFnDclNode(Name *namesym, uint16_t tag, INode *sig, INode *val);

// Create a new overloaded function/method declaration node
FnOverloadDclNode *newFnOverloadDclNode(Name *namesym);

// Append a concrete declaration to an overload set's ordered candidates
void fnOverloadDclAdd(FnOverloadDclNode *ovlnode, FnDclNode *fnnode);

// Return a clone of a function/method declaration
INode *cloneFnDclNode(CloneState *cstate, FnDclNode *oldfn);

void fnDclPrint(FnDclNode *fn);

void fnOverloadDclPrint(FnOverloadDclNode *fn);

/// Resolve all names in a function
void fnDclNameRes(AnalysisState *pstate, FnDclNode *name);

// Type checking a function's logic, does more than you might think:
// - Turn implicit returns into explicit returns
// - Perform type checking for all statements
// - Perform data flow analysis on variables and references
void fnDclTypeCheck(AnalysisState *pstate, FnDclNode *fnnode);

// Verify no two candidates of an overload set accept the same parameter signature.
// Candidates are not walked, as each is separately checked by its owning module or type.
void fnOverloadDclTypeCheck(AnalysisState *pstate, FnOverloadDclNode *node);

#endif
