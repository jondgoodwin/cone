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
    Nodes *moduses;          // Every standalone 'use' written at module scope, of an enum or a submodule, expanded by modFoldNames
    Nodes *nodes;            // All parsed nodes owned by the module
    Namespace namespace;     // The module's named nodes, owned or "used"
    DclInfo dclinfo;         // Owner and the facts that decide the linker symbols it prefixes
    uint16_t foldpass;       // The fold pass (modFoldAll) that last reached this module; 0 for none yet
    uint16_t folding;        // Its folds are running in that pass: a module reaching it now closes a loop (only one refused already)
    uint16_t dagmark;        // The module-order walk (pgmModuleOrder): 0 unvisited, 1 on the walk's path, 2 placed in the order
    INode *extendsname;      // 'mod A extends B': B as written, a NameUseNode; NULL where the module extends nothing
    struct ImportNode *extends; // The fold 'extends' makes of B's names, once B resolves (modExtendsResolve); else NULL
    struct FoldClause *deffold; // 'mod A use B': what a bare import of this module folds by default; NULL where the line has no 'use'
    INode *traitname;        // 'mod A is T': T as written, a NameUseNode bound to the module trait once modTraitConform finds it; else NULL
    struct ModTraitNode *trait; // The module trait 'is' names, once resolved; else NULL
    uint32_t ntaken;         // How many of 'nodes', at its end, are copies of the trait's defaults, made resolved (modTraitConform)
    struct FnDclNode *initfn; // The module's own 'init', once modLifecycle has checked it; else NULL
    struct FnDclNode *finalfn; // What finalizes the module: the 'drop' modLifecycle gives it where a global needs dropping, else its own 'final'; else NULL
    GenericInfo *genericinfo; // 'mod stack[T]': a generic module, its type parameters and its instances; else NULL
    struct ModuleNode *generic; // An instance of a generic module: the generic it was cloned from; else NULL
    Nodes *instdeps;         // An instance: the modules it follows in the init order besides its generic (pgmInstanceOrder); else NULL
    DclSpans *spans;         // Where each of its statements sits in its files, in the order parsed (dclspan.h); NULL for none
} ModuleNode;

ModuleNode *newModuleNode();
void modPrint(ModuleNode *mod);
void modAddNode(ModuleNode *mod, Name *name, INode *node);
void modAddNamedNode(ModuleNode *mod, Name *name, INode *node);

// Add a parsed function to the module, binding its unique concrete name and,
// when it declares an overload name, that name's separate FnOverloadDclNode
void modAddFn(ModuleNode *mod, FnDclNode *fnnode);

void modHook(ModuleNode *oldmod, ModuleNode *newmod);

// Put every name each of the program's modules holds by FOLDING into its
// namespace: what its 'extends' and its imports admit, what its globals' 'use'
// clauses do, and the variants its 'use' statements fold in from enums. Ahead of
// any module's name resolution, and dependency-first, so that what a name
// reaches through a qualifier does not depend on the order the files were
// loaded in. Where a pass leaves a fold waiting -- a global whose type a later
// fold of its own module brings -- the folds run again until a pass binds
// nothing new; what is still missing then is reported, once. Imports form a DAG
// (pgmModuleOrder); round a loop already refused, the same passes carry the names
// on, so the loop is the one thing reported.
void modFoldAll(NameResState *pstate, Nodes *modules);

// Fold one module parsed after every module of 'modules' was resolved -- the
// include-file generator's self-check -- without folding any of those again
void modFoldAlone(NameResState *pstate, Nodes *modules, ModuleNode *mod);

// Run one module's folds for the current pass, if they have not run in it
// already. Reached from modFoldAll, and by demand from a global's fold that
// needs a struct of another module resolved (structNameResDemand)
void modFoldNames(NameResState *pstate, ModuleNode *mod);

// Is this the pass that reports? Until it, a fold that cannot be made yet --
// a name not there, or not public -- waits (modFoldWait), since a later pass may
// bring it; in it, the fold reports what it could not make
int modFoldReporting();

// Record that a fold could not be made in this pass and waits for a later one
void modFoldWait();

// Would this source -- a name, a path, a reference to either -- name nothing
// yet, because the lookup at some step finds no binding? A global's fold or a
// standalone 'use' naming its source through a re-export still to arrive waits for
// it rather than resolving the name and reporting it missing
int modFoldAwaits(INode *source);

// Where to report a name a fold brings in that this module already binds to
// something else: at the new binding, unless what holds the name was made by a
// fold of this module that runs AFTER 'unit' ('extends' first, then the imports,
// the globals and the standalone 'use's, each in the order written). A later
// pass can bind a later fold's name before an earlier fold's arrives, and the
// collision is then reported where a single pass would have met it
INode *modFoldCollisionAt(ModuleNode *mod, INode *unit, INode *alias, INode *prior);

// Bind a name a fold brings into a module's namespace, and hook it. NULL once
// bound, or where the name is bound already to the same declaration by the same
// route and one of the two was not written by the module (a star clause made it)
// -- the binding is then public if either route is; otherwise the binding that
// holds the name, for the caller to report as a collision
struct AliasDclNode;
INode *modFoldBind(ModuleNode *mod, struct AliasDclNode *alias);

// Do two bindings stand for the same thing -- the same declaration, reached
// through the same global, if any -- so that one binding is the other?
int modFoldSameBinding(INode *a, INode *b);

// Report the binding modFoldBind returned as a collision, at 'at'
// (modFoldCollisionAt): a name written twice for the same thing, or a name
// meaning two things
void modFoldDupReport(INode *at, struct AliasDclNode *alias, INode *prior);

// Resolve what a module's 'extends' names, refusing what cannot be reused. Run
// for every module ahead of any fold, since what a module extends is folded first
void modExtendsResolve(ModuleNode *mod);

// Make, register and type check the instance of generic module 'generic' for
// the type arguments 'srcgencall' gives: a module of its own, cloned from the
// generic with each parameter substituted, owned where the generic is and
// spelled with the arguments ('stack[i64].push'). genericMemoize asks for it on
// a memo miss, so identical arguments anywhere are one instance
ModuleNode *modInstantiate(TypeCheckState *pstate, struct FnCallNode *srcgencall, ModuleNode *generic);

// Every instance of a generic module made so far, in the order made, or NULL
Nodes *modInstanceList();

void modNameRes(NameResState *pstate, ModuleNode *mod);
void modTypeCheck(TypeCheckState *pstate, ModuleNode *mod);

// The module whose own 'init' this function is -- a function the module owns,
// not a method, named 'init' -- or NULL. Asked by fnDclTypeCheck around the
// function's data flow pass, which is where 'init''s globals are checked
ModuleNode *modInitOf(FnDclNode *fnnode);

// Around the data flow pass of a module's 'init': each global the module declares
// without an initial value starts the pass unassigned, so 'init' may assign it
// once -- an 'imm' one included -- and may not read it before it does. After the
// pass, each one it never saw assigned is ErrorGlobalUninit, and every global
// holds a value again for every other function. 'saved' carries their flags
uint16_t *modInitFlowBegin(ModuleNode *mod);
void modInitFlowEnd(ModuleNode *mod, uint16_t *saved);

#endif
