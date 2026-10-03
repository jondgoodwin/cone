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
    DclInfo dclinfo;              // Owner and the facts that decide the linker symbol (name.c spells it)
    GenericInfo *genericinfo;     // Link to generic parms, etc (or NULL if not generic)
    Nodes *where;                 // Its constraints, conditions all required (generic.h), or NULL
    uint16_t vtblidx;             // Method ptr's index in the type's vtable
    // A compute entry point's workgroup size, '@compute(x, y, z)', each 1 when
    // not written; all 0 for any other function (fnDclIsCompute)
    uint16_t compute[3];
} FnDclNode;

// Whether a function is a compute entry point, '@compute(...)'
#define fnDclIsCompute(fn) ((fn)->compute[0] != 0)

// Why 'type' may not be in GPU memory -- a kernel's buffer, a '@workgroup'
// global -- or NULL when it may: 32-bit numbers, their atomics, and structs and
// fixed arrays of them. 'path' names the field or element at fault, appended to
// in place
const char *fnDclComputeData(INode *type, char *path, size_t size);

// Overloaded function/method declaration node.
// It is the namespace binding for an explicitly declared overload name.
// It has no type, value, or generated symbol: every executable implementation
// remains a separate FnDclNode found in 'overloads', bound to its own concrete name.
typedef struct FnOverloadDclNode {
    INodeHdr;
    Name *namesym;
    Nodes *overloads;             // Ordered list of FnDclNode candidates
} FnOverloadDclNode;

// Create a new function declaraction node. The tag is always FnDclTag; the
// second argument is the node's flags (FlagMethFld and friends)
FnDclNode *newFnDclNode(Name *namesym, uint16_t flags, INode *sig, INode *val);

// Create a new overloaded function/method declaration node
FnOverloadDclNode *newFnOverloadDclNode(Name *namesym);

// Append a concrete declaration to an overload set's ordered candidates,
// unless it is private and the set's name is public (ErrorPrivOverload)
void fnOverloadDclAdd(FnOverloadDclNode *ovlnode, FnDclNode *fnnode);

// Return a clone of a function/method declaration
INode *cloneFnDclNode(CloneState *cstate, FnDclNode *oldfn);
// The same in two steps: the copy with its original's signature and body, then those copied
FnDclNode *cloneFnDclShell(FnDclNode *oldfn);
void cloneFnDclFill(CloneState *cstate, FnDclNode *newnode, FnDclNode *oldfn);

void fnDclPrint(FnDclNode *fn);

void fnOverloadDclPrint(FnOverloadDclNode *fn);

// Whether an importer expands this function's body in its own object rather than
// calling a symbol: inline, generic, or a default or method that 'typenode' --
// the type or module trait whose braces declare it, or NULL -- copies. Name
// resolution asks it, and so does the include-file generator
int fnDclIsExpanded(FnDclNode *fndclnode, INode *typenode);

// Is this an initializer: a method whose 'self' is '&new', a reference to the
// memory its value is to be written into? A construction runs one, and so does
// an allocation, on a region's header and a permission's.
int fnDclIsInit(FnDclNode *fn);

/// Resolve all names in a function
void fnDclNameRes(NameResState *pstate, FnDclNode *name);

// Type checking a function's logic, does more than you might think:
// - Turn implicit returns into explicit returns
// - Perform type checking for all statements
// - Perform data flow analysis on variables and references
void fnDclTypeCheck(TypeCheckState *pstate, FnDclNode *fnnode);

// Verify no two candidates of an overload set accept the same parameter signature.
// Candidates are not walked, as each is separately checked by its owning module or type.
void fnOverloadDclTypeCheck(TypeCheckState *pstate, FnOverloadDclNode *node);

#endif
