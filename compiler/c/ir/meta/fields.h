/** Reflection over a type's declared fields: '#each #f in #fields(T)'
 *
 * The parser reads the loop's body once, with '#f' bound to a FieldEach. The body is a template
 * that is name resolved once (like any generic's); cloning it for an instance of T makes one block
 * per field of T, with each '#f.name' and the like replaced by what that field says and each
 * 'val.#f' a member access of the field's name. Only the copies are type checked.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef fields_h
#define fields_h

// What a field description can be asked: '#f.name' (text), '.type', '.region', '.perm', '.pub'
typedef enum {
    FieldPropName,
    FieldPropType,
    FieldPropRegion,
    FieldPropPerm,
    FieldPropPub,
    FieldPropOffset,    // The field's byte offset in T: a layout fact, a usize the code generator reads
    FieldPropCount
} FieldProp;

typedef struct FieldEach {
    Name *var;                          // '#f', as the loop binds it
    INode *subject;                     // T: the type parameter whose fields are described
    Name *propnames[FieldPropCount];    // '#f.name', ... : the names the placeholders carry
    GenVarDclNode *props[FieldPropCount];   // ... and what they are bound to until the loop is cloned
    int used[FieldPropCount];           // Which properties the body asks of a field
    ModuleNode *mod;                   // The module the reflecting code is written in
} FieldEach;

// The name of a member written in a '#each #f in #fields(T)' in a generic struct's braces: the
// described field's name, with a fixed word before or after it (never built from anything else)
typedef struct GenName {
    FieldEach *fe;      // The loop whose '#f' it joins
    char *prefix;       // '#name("get", #f)': "get"; else NULL
    char *suffix;       // '#name(#f, "Count")': "Count"; else NULL
} GenName;

// A loop over the fields of 'subject', whose code is written in 'mod'
FieldEach *newFieldEach(Name *var, INode *subject, ModuleNode *mod);

// The placeholder for '#f.prop' in the template, standing where lexnode is
INode *fieldEachProp(FieldEach *fe, FieldProp prop, INode *lexnode);

// The word after '#f.' as a property, or FieldPropCount where it is none (-1 for the retired 'attrs')
int fieldPropFromWord(const char *word);

// Name resolution of the loop's block (its subject): it must be a type parameter
void fieldEachNameRes(NameResState *pstate, BlockNode *blk);

// Clone the loop's block for an instance: one block of the body for each field
INode *cloneFieldEach(CloneState *cstate, BlockNode *blk);

// A call's argument list cloned for an instance: a '#each #f in #fields(T) { #f: value }' among the
// arguments (a BlockNode with 'fieldeach', its one statement the NamedValNode) becomes one named
// argument per field; an undecided one is kept as written. Other arguments are cloned as they are.
Nodes *cloneFieldEachArgs(CloneState *cstate, Nodes *args);

// A loop being expanded for one instance: T's fields as the reflecting code may see them, and the
// names a pass hooks while its copy is made (see fieldEachPassBegin)
typedef struct FieldEachRun {
    FieldEach *fe;
    INode *subj;            // The instance's T, as cloned
    Nodes *fields;          // The FieldDclNodes of T it describes, in order
    int undecided;          // T is still a parameter of an enclosing generic: nothing is expanded
    int failed;             // An error was reported: nothing is expanded
    INode *oldhook[FieldPropCount];
    INode *oldvar;
    FieldEach *kept;        // Undecided: the loop's copy, over the instance's T, that the members kept as written hold
} FieldEachRun;

// An undecided loop's member kept as written: its name's GenName, joined to the run's own copy of the
// loop (over the copy's T), so that each instance of the copy decides it for its own T
// Hook a loop's properties to themselves while its members are copied as written, so a copy of '#f.type' is a use of the same placeholder
void fieldEachKeepBegin(FieldEachRun *run);

GenName *fieldEachKeptGen(FieldEachRun *run, GenName *gen);

// Decide T for an instance (the caller holds a cloneDclPush); errors are reported at 'errnode'
FieldEachRun *fieldEachOpen(CloneState *cstate, FieldEach *fe, INode *errnode);

// Hook '#f' and its properties to field 'i' of T, for a copy made now; 0 where a property cannot be
// answered (reported at 'at')
int fieldEachPassBegin(FieldEachRun *run, uint32_t i, INode *at);

// Put the hooks back as they were
void fieldEachPassEnd(FieldEachRun *run);

// The pass's EachPass, for an error to name the pass
EachPass *fieldEachPassFor(FieldEachRun *run, uint32_t i, INode *loop);

// The name a generated member has for field 'field' of T
Name *genNameFor(GenName *gen, FieldDclNode *field);

// Name resolution of a loop's subject: it must be a type parameter
void fieldEachSubjectNameRes(NameResState *pstate, FieldEach *fe);

// A member name '#f' met while cloning (the splice 'val.#f'): the field's name while a loop is
// being expanded, else NULL
INode *fieldEachSplice(NameUseNode *node);

#endif
