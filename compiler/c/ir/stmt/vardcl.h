/** Handling for variable declaration nodes
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef vardcl_h
#define vardcl_h

// Variable declaration node (global, local, parm)
//
// 'fold' serves name folding and is empty everywhere but on a module's global.
// A global is the one-instance analogue of a field, so its 'use' clause admits
// names of its type as names of the module -- reached through the global, whose
// address is fixed at compile time (compiler/c/doc/nodes/module.md, "Name folding").
typedef struct VarDclNode {
    IExpNodeHdr;             // 'vtype': type of this name's value
    Name *namesym;
    INode *value;              // Starting value/declaration (NULL if not initialized)
    LLVMValueRef llvmvar;      // LLVM's handle for a declared variable (for generation)
    DclInfo dclinfo;           // Owner and the facts that decide the linker symbol (globals; name.c spells it)
    INode *perm;               // Permission type (often mut or imm)
    struct FoldClause *fold;   // A global's fold clause, or NULL
    uint16_t scope;            // 0=global
    uint16_t index;            // index within this scope (e.g., parameter number)
    uint16_t flowflags;        // Data flow pass permanent flags
    uint16_t flowtempflags;    // Data flow pass temporary flags
    Nodes *hollowed;           // Data flow: each move that took a part out of what this owning reference points at
} VarDclNode;

enum VarFlowTemp {
    VarInitialized = 0x0001,    // Variable has been initialized
    VarMoved = 0x0002,          // Variable has been moved
    VarHollow = 0x0004          // A part of what this owning reference points at was moved out ('hollowed')
};

VarDclNode *newVarDclNode(Name *namesym, uint16_t tag, INode *perm);
VarDclNode *newVarDclFull(Name *namesym, uint16_t tag, INode *sig, INode *perm, INode *val);

// Create a new variable dcl node that is a copy of an existing one
INode *cloneVarDclNode(CloneState *cstate, VarDclNode *node);
// The same in two steps: the copy with its original's type and value, then those copied
VarDclNode *cloneVarDclShell(VarDclNode *node);
void cloneVarDclFill(CloneState *cstate, VarDclNode *newnode, VarDclNode *node);

void varDclPrint(VarDclNode *fn);

// Name resolution of vardcl
void varDclNameRes(NameResState *pstate, VarDclNode *node);

// Type check vardcl
void varDclTypeCheck(TypeCheckState *pstate, VarDclNode *node);

// Perform data flow analysis
void varDclFlow(FlowState *fstate, VarDclNode **vardclnode);

#endif
