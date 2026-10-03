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
    uint16_t flowtempflags;    // Data flow pass temporary flags
    uint16_t flowdepth;        // Data flow: the conditional depth it was declared at (flowDepth)
    uint8_t flowtracked;       // Data flow: 0 not yet asked, 1 its state is not followed for drops, 2 it is (flowDropTracked)
    uint32_t flowindex;        // Transient: this variable's index in the loan walk (flowpath.c), 0 outside one
    Nodes *hollowed;           // Data flow: each move that took what this owning reference points at, or an element of it, out
    Nodes *hollowall;          // Data flow: every such move in the function, on any path (a drop flag's hollow release)
    LLVMValueRef llvmflag;     // Generation: its drop flag, for a variable with VarDropFlag
} VarDclNode;

enum VarFlowTemp {
    VarInitialized = 0x0001,    // Variable has been initialized
    VarMoved = 0x0002,          // Variable has been moved
    VarHollow = 0x0004,         // What this owning reference points at, or an element of it, was moved out ('hollowed')
    VarDropFlag = 0x0008,       // Whether it holds its value differs by path: a drop flag says so at run time
    // An initializer's 'self &new' whose value has not been written yet on some
    // path: set as its function's flow begins, cleared by '*self = value'. Like
    // the others it is joined by union, so a store on only some paths leaves it.
    VarUnfilled = 0x0010
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

// A temporary a borrow in a local's initializer is rooted in, made a hidden
// local in case the borrow extends it (vardcl.c, "Temporaries an initializer
// extends")
typedef struct VarDclTemp {
    VarDclNode *var;    // the hidden local, whose value is the temporary's expression
    INode **slot;       // where that expression was: now a name use of the local
    uint8_t kept;       // the borrow extends it
} VarDclTemp;

// A block's statement declaring a local, while it is type checked
struct VarDclExtend {
    VarDclExtend *outer;    // the statement this one's block is within, if any
    VarDclNode *var;        // the local the statement declares
    VarDclTemp *temps;
    uint32_t ntemps;
    uint32_t tempcap;
    INode ***pending;       // elements of a literal run before the next hidden local, still in place
    uint32_t npending;
    uint32_t pendcap;
    uint32_t flushes;       // how many times 'pending' was made hidden locals
    uint32_t ran;           // how many hidden locals moved code ahead of where it was written
    Nodes *hoisted;         // the hidden locals declared before the statement, in the order they run
    Nodes *tail;            // within a block's final expression, what runs before it there
    uint8_t inblock;        // extending within a block's final expression (an 'if''s arm, a 'match''s)
};

// Type checking a block's statement declaring 'var' begins and ends. The end
// hands back the hidden locals to declare before it, in order, or NULL.
void varDclExtendBegin(TypeCheckState *pstate, VarDclExtend *ext, VarDclNode *var);
Nodes *varDclExtendEnd(TypeCheckState *pstate, VarDclExtend *ext);

// A borrow, type checked within such a statement, of a place whose root
// 'slot' is a temporary: make the temporary a hidden local, which the borrow
// then borrows, and which goes back in place, a temporary of the statement,
// where no extending borrow reaches it. Returns 0, changing nothing, outside
// such a statement.
int varDclExtendTemp(TypeCheckState *pstate, INode **slot);

#endif
