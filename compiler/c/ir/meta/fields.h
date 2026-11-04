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

// A member name '#f' met while cloning (the splice 'val.#f'): the field's name while a loop is
// being expanded, else NULL
INode *fieldEachSplice(NameUseNode *node);

#endif
