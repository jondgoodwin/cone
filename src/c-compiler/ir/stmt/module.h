/** Module and import structures and helper functions
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef module_h
#define module_h

// Module is a global namespace owning (or folding in) many kinds of named nodes:
// - Functions
// - Global variables
// - Type declarations (incl. permissions and allocators)
// - Macro and template definitions
// - Modules nested within this parent module (and names folded in from them)
//
// A module supports forward referencing of its names (except permissions and allocators).
// It also supports name resolution of namespace qualified, public names.
typedef struct ModuleNode {
    IExpNodeHdr;
    Name *namesym;           // The module's name: its folder's for a module folder, else its file's
    Name *filesym;           // The name derived from the module's filename
    Name *foldersym;         // The module's folder, when its designated file drew it; else NULL
    Nodes *imports;          // All import nodes
    Nodes *enumuses;         // Every 'use' of an enum written at module scope, expanded by modFoldNames
    Nodes *nodes;            // All parsed nodes owned by the module
    Namespace namespace;     // The module's named nodes, owned or "used"
    DclInfo dclinfo;         // Owner and the facts that decide the linker symbols it prefixes
    uint16_t foldstate;      // How far modFoldNames has got: 0 not begun, 1 running, 2 done
    INode *extendsname;      // 'mod A extends B': B as written, a NameUseNode; NULL where the module extends nothing
    struct ImportNode *extends; // The fold 'extends' makes of B's names, once B resolves (modExtendsResolve); else NULL
} ModuleNode;

ModuleNode *newModuleNode();
void modPrint(ModuleNode *mod);
void modAddNode(ModuleNode *mod, Name *name, INode *node);
void modAddNamedNode(ModuleNode *mod, Name *name, INode *node);

// Add a parsed function to the module, binding its unique concrete name and,
// when it declares an overload name, that name's separate FnOverloadDclNode
void modAddFn(ModuleNode *mod, FnDclNode *fnnode);

void modHook(ModuleNode *oldmod, ModuleNode *newmod);

// Put every name this module holds by FOLDING into its namespace: what its
// imports admit, what its globals' 'use' clauses do, and the variants its
// 'use' statements fold in from enums. Ahead of any module's
// name resolution, and dependency-first, so that what a name reaches through a
// qualifier does not depend on the order the files were loaded in
void modFoldNames(NameResState *pstate, ModuleNode *mod);

// Report that 'name' is missing from 'mod', looked up from 'reader', with the
// diagnostic given -- unless the cause is a re-export lost round a cycle of
// imports, which modFoldNames does not carry round and which this reports as
// ErrorCircular instead, naming the cycle. The diagnostic given is reported as
// it is wherever the cause is anything else.
void modNameMissing(ModuleNode *reader, ModuleNode *mod, Name *name, INode *at, int code, const char *msg, ...);

// Bind a name a fold brings into a module's namespace, and hook it. NULL once
// bound, or where the name is bound already to the same declaration by the same
// route -- the binding is then public if either route is; otherwise the binding
// that holds the name, for the caller to report as a collision
struct AliasDclNode;
INode *modFoldBind(ModuleNode *mod, struct AliasDclNode *alias);

// Resolve what a module's 'extends' names, refusing what cannot be reused. Run
// for every module ahead of any fold, since what a module extends is folded first
void modExtendsResolve(ModuleNode *mod);

// Refuse a module whose chain of 'extends' comes back to it, and cut the chain
// there. 'nmods' bounds the walk, which a cycle not through this module would
// otherwise never leave
void modExtendsCheckCycle(ModuleNode *mod, uint32_t nmods);

void modNameRes(NameResState *pstate, ModuleNode *mod);
void modTypeCheck(TypeCheckState *pstate, ModuleNode *mod);

#endif
