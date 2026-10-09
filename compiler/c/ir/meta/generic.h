/** Handling for generic nodes (also used for macros)
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef generic_h
#define generic_h

typedef struct GenericInfo {
    Nodes *parms;            // Declared parameter nodes w/ defaults (GenVarTag)
    Nodes *memonodes;        // Pairs of memoized generic calls and cloned bodies
    Nodes *where;            // A generic type's constraints: conditions, all required (below)
} GenericInfo;

// Create a new generic info block
GenericInfo *newGenericInfo();

// Name resolution of a generic function's or type's type parameters, inside
// the hooked context its caller pushed: all are hooked first, then what follows
// each is resolved, so a bound may name a parameter declared after it
void genericParmsNameRes(NameResState *pstate, Nodes *parms);

// Serialize
void genericInfoPrint(GenericInfo *info);

// Obtain the GenericInfo a declaration carries, or NULL if it is not a generic.
// This is what distinguishes a generic from every other declaration: a generic
// is an ordinary FnDcl, StructNode or ModuleNode with a parameter list
// attached, and not a declaration node of its own.
GenericInfo *genericGetInfo(INode *node);

// Bound the nesting of generic instantiation and macro expansion.
//
// Both work by cloning a declaration and analyzing the clone, and analyzing it
// can expand the same declaration again at larger arguments. Every expansion is
// a distinct node, so the TypeChecking mark never sees such a cycle -- nothing ever
// returns to the same node. Depth is the only thing that tells a generic that
// terminates from one that does not, and past the limit it is the C stack the
// walk runs on that gives out, with no diagnostic at all.
//
// Enter reports and returns 0 when the limit is reached; the caller substitutes
// an error node for what it could not expand. From then until the outermost
// expansion has unwound, Enter returns 0 without a report: what the levels
// above try next is not started, so an expansion that branches still ends.
// Every successful Enter is paired with an Exit once the expansion has been
// analyzed.
int genericInstantiateEnter(INode *errnode);
void genericInstantiateExit();

// The expansion depth, kept with a layout a reference's target left waiting and
// restored while it is laid out: an instance laid out later is still that deep,
// so an expansion through references ('next &Box[Box[T]]') is bounded too
uint32_t genericInstantiateDepth();
void genericInstantiateDepthSet(uint32_t depth);

// Perform generic substitution, if this is a correctly set up generic "fncall"
// Return 1 if done/error needed. Return 0 if not generic or it leaves behind a lit/fncall that needs processing.
int genericSubstitute(TypeCheckState *pstate, FnCallNode **nodep);

// The instance of generic method 'genmeth' that a call on a receiver names,
// its type arguments 'typeargs' or, when NULL, inferred from the call's
// arguments. NULL once an error is reported.
FnDclNode *genericMethodInstance(TypeCheckState *pstate, FnCallNode *callnode, FnDclNode *genmeth, Nodes *typeargs);

// Is 'fn' one of the instances made of generic function or method 'generic'?
int genericIsInstanceOf(INode *fn, FnDclNode *generic);

// The signature a closure literal given as argument 'argi' of a call of this
// generic function or method is to fit (closure.h); see generic.c
FnSigNode *genericParmBound(FnDclNode *generic, uint32_t pos, INode **refperm);

// Refuse, at the caller's argument, a callable whose '()' changes its state given
// to a generic parameter taken as '&F'. 0 once reported.
int genericCallablePermCheck(FnDclNode *generic, Nodes *valueargs, uint32_t firstparm);

// Append a checked signature as a message spells it: 'fn(&Person, &Person) i32'
void genericFnSigCat(char *buf, size_t size, FnSigNode *sig);
FnSigNode *genericClosureSig(TypeCheckState *pstate, FnDclNode *generic, Nodes *args, uint32_t firstparm,
        uint32_t argi, INode **refperm);

// Constraints: 'where T is Name and ... or ...', and the inline '[T Name + Name]'.
//
// A 'where' list holds conditions, every one of which must hold: the operands
// of the 'and's at the top of what was written, each in its own element. A
// condition is a clause, 'T is Name' -- an IsTag CastNode whose 'exp' is a use
// of the type parameter it constrains and whose 'typ' is a use of the trait --
// or an OrLogicTag or AndLogicTag LogicNode joining two conditions. A generic
// type's own list is its GenericInfo's; a function's -- a generic function's
// requirements, a generic type's method's conditions -- is its FnDcl's. Each
// condition is checked by evaluating it once the parameters' arguments are
// known, never solved and never used to infer.

// Resolve a declaration's constraints, once its type parameters are hooked:
// fold what each parameter's annotation names into 'where' as clauses of its
// own, and resolve and vet every clause, dropping any condition with a clause
// whose subject is not a type parameter of this generic, or of the type it is
// a member of, or whose name is not a trait
void genericConstraintsNameRes(NameResState *pstate, Nodes *parms, Nodes **wherep);

// Resolve and vet the condition on one entry of a generic type's 'is' list,
// 'is Move if T is Move', once its type parameters 'parms' are hooked: a
// condition as a 'where' clause writes one, each clause asking about one of
// 'parms'. Returns 0, reported, where any clause is refused. An instance has
// the entry where its arguments make the condition true (genericInstantiate
// evaluates it, as it does a method's 'where').
int genericIsConditionNameRes(NameResState *pstate, Nodes *parms, INode *cond);

// Is 'type' what 'trait' says, as a constraint asks it? A marker trait -- one
// requiring nothing of a value, the compiler's own among them -- by the
// compiler's grant or an 'is' declaration only; any other trait by a
// declaration or by fitting it structurally
int genericTypeIs(INode *type, StructNode *trait);

// Judge again each instance whose 'T is Sendable' was met while a struct its
// argument reaches was not yet laid out, and refuse one that is not after all.
// Called once, when type check has finished.
void genericSendableCheckAll();

// Why a type is not Sendable, as the thread check's diagnostics say it: where
// the culprit sits in it, into 'what', and what kind of thing it is, into
// 'reason', each 512 bytes. Returns whether the cause is a borrow or a
// permission, which a diagnostic says is not a local's own 'mut'. 'how' is how
// the check was made: whether a borrow of the whole program may cross there
int genericNotSendableWhy(INode *arg, char *what, char *reason, StaticBorrow how);

// When the method or function 'name' is absent from the generic type instance
// 'typedcl' because its 'where' clause is not met there, report so at
// 'errnode', naming the clause, and return 1. Otherwise return 0.
int genericReportAbsent(INode *errnode, INode *typedcl, Name *name);

// Is this function a member of a generic type's template that exists only
// where its clause holds? A use of one from an instance is a use of what that
// instance does not have; report it at 'errnode' and return 1.
int genericReportTemplateMember(INode *errnode, FnDclNode *fn);

#endif
