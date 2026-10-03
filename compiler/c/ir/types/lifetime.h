/** Named lifetimes: on a function's signature, on a struct, and ordered
 * @file
 *
 * A borrowed reference type written in a function's signature may name its
 * lifetime, right after the '&': '&'a T', '&'a mut T', '&[]'a u8'. Each name is
 * one lifetime of the caller's, and a borrow written with none has the unnamed
 * lifetime, one more name, shared by every unannotated borrow in the signature
 * -- and by every borrow an unannotated value of another type holds (a field of
 * a struct that declares no lifetimes). ''static' is the global lifetime: it
 * outlives every name and ties nothing to anything.
 *
 * A struct (or enum) may declare lifetimes in its bracket list, apart from its
 * type parameters: 'struct Cursor['a] { items &[]'a R; }'. Its fields name them
 * and nothing else; a struct whose fields name one lifetime and whose brackets
 * declare none takes that one as declared. A lifetime changes no layout and no
 * code, so it is never instanced: 'Cursor['a]' and 'Cursor' are one type. A use
 * names the struct's lifetimes positionally in brackets ('Cursor['a]',
 * 'Parser['s, 'r]'), before any type arguments ('Cursor['a, T]'); a use naming
 * none gives each the unnamed lifetime; and 'Self', inside the struct's
 * methods, names its own, so that a method's result may point where a field
 * points ('&'a R'), not into the value. A struct holding no borrow holds no
 * lifetime, whatever it declares.
 *
 * A name means something only in the signature it is written in, so names are
 * compared by identity, and only between the types of one signature: a
 * callee's own, while its body is checked, or the one a call is made through.
 * Two names are unordered unless the signature's 'where' clause orders them
 * ('where 'a >= 'b': ''a' lasts at least as long as ''b'), or the 'where' clause
 * of a struct a parameter or the result uses does, or a type in it implies it:
 * a borrow of a value holding lifetimes ('&'a Pair['b]', '&'a &'b T', 'self &'
 * of a struct declaring ''b') cannot outlast them, so each lasts at least as
 * long as the borrow's own (Rust's implied bounds), the unnamed lifetime a
 * name like any other there. '>=' is transitive, and nothing else is
 * inferred. RefNode.lifename holds a reference's name (NULL for
 * the unnamed lifetime); RefNode.scope stays the band (0 global, 1 the caller's,
 * 2+ a block of the function), which the names divide no further.
 *
 * What a caller lends through a parameter is held apart in parts: what the
 * parameter's own reference points at ('LifePartOwn', of its lifetime), and
 * what that holds -- or what a parameter passed by value holds --, as a whole
 * ('LifePartHeld') or, for a struct declaring lifetimes, one part per lifetime
 * ('LifePartSlot' + k, its k-th, a "slot"). The flow walk keeps a caller loan
 * per part, and a call carries an argument's loans by the parts whose
 * lifetimes the result may hold (lifeCarry). compiler/c/doc/phases/flow.md,
 * "Named lifetimes", is the note.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef lifetime_h
#define lifetime_h

struct FnSigNode;
struct FnDclNode;
struct StructNode;
struct FieldDclNode;
struct NameUseNode;

// At most this many lifetimes a struct declares: a slot is a bit of a mask
#define LifeMaxSlots 31
#define LifeAllSlots 0xFFFFFFFFu

// An order among lifetimes: each pair says its first lasts at least as long as
// its second ('a >= 'b'); '==' is two pairs. 'at' holds where each pair was
// written, for a diagnostic.
typedef struct LifeOrder {
    Name **pairs;       // 2 * count: longer, shorter
    INode **at;         // count
    uint32_t count;
    uint32_t cap;
} LifeOrder;

// The lifetimes a struct declares (StructNode.lifeparms), shared by its
// variants and by every instance of a generic one
typedef struct LifeParms {
    Name **names;       // declared, in order: slot k is names[k]
    uint32_t *contains; // per slot: the slots whose borrows its places may hold, by the struct's order
    LifeOrder *order;   // its 'where' clause's lifetime comparisons, in its own names
    Nodes *usedat;      // while it is parsed: each name a field writes, as a name use where written
    uint16_t count;
    uint8_t inferred;   // the one name its fields write, not declared in brackets
} LifeParms;

// The lifetimes a type's use names (NameUseNode.lifeuse): a struct's declared
// ones, positionally ('Cursor['a]'), and a generic instance's type arguments as
// written, where they hold a borrow ('Option[&'a R]': one instance serves every
// use, whatever lifetimes it names, so they are read from the use)
typedef struct LifeUse {
    Name **names;       // positional names for the struct's declared lifetimes, or NULL
    Nodes *typeargs;    // a generic instance's type arguments, or NULL
    INode *at;          // where the names are written
    Name *held;         // a bounded type argument's: the one lifetime every borrow it holds is of (lifeRenamed), or NULL
    uint16_t count;
} LifeUse;

// The parts a caller lends through a parameter (above)
enum LifePart {
    LifePartOwn = 0,
    LifePartHeld = 1,
    LifePartSlot = 2,   // + k
};

// How a value of one type may hold what an argument carries (lifeCarry)
enum LifeCarry {
    LifeCarryNone,      // none of it
    LifeCarryWhole,     // all of it: the parameter's own lifetime is one it holds
    LifeCarryHeld,      // only what the argument's reference points at holds, or, by slot, what a value holds
};

LifeOrder *newLifeOrder();
void lifeOrderAdd(LifeOrder *order, Name *longer, Name *shorter, INode *at);

// A struct's lifetimes, none declared yet
LifeParms *newLifeParms();

// Declare one more of a struct's lifetimes, written at 'at': 0 for a name
// declared twice, or one too many
int lifeParmsDeclare(LifeParms *parms, Name *name, INode *at);

// A copy of a type with no lifetime named in it, outside a function type's
// signature, and no band (RefNode.scope) on any reference in it: what a
// generic is instanced at, since a lifetime is never instanced
INode *lifeErased(INode *type);

// Does a value of this type hold a borrow of the lifetime 'life' (NULL for the
// unnamed one)? A borrow of ''static' is held by nothing in this sense: it ties
// the value to no caller lifetime.
int lifeHolds(INode *type, Name *life);

// May a borrow held by a value of type 'from' be held by one of type 'to', in
// the signature 'sig': does 'from' hold a lifetime lasting at least as long as
// one 'to' holds, by the signature's order?
int lifeFlows(struct FnSigNode *sig, INode *from, INode *to);

// The struct with declared lifetimes a type (looking through a name) is, or NULL
struct StructNode *lifeSlotted(INode *type);

// The lifetime a use of a struct with declared lifetimes gives its slot 'k'
Name *lifeSlotName(INode *use, uint32_t k);

// What a parameter of this type holds beyond its own reference: what a
// borrowed reference points at, or the value itself; NULL where that holds no
// borrow
INode *lifeHeld(INode *parmtype);

// Is this a borrowed reference whose own lifetime a caller lends: no
// function's, no ''static'?
int lifeIsOwnBorrow(INode *parmtype);

// How a value of the type 'to' may hold what the argument for a parameter of
// the type 'parm' carries, in the signature 'sig' (NULL: an unannotated one,
// every borrow in it sharing one lifetime, so always the whole). For
// LifeCarryHeld, '*slots' is the mask of the held struct's slots whose
// lifetimes 'to' may hold, LifeAllSlots for a held value declaring none.
int lifeCarry(struct FnSigNode *sig, INode *parm, INode *to, uint32_t *slots);

// May a value of the type 'to' hold the part 'part' of what a caller lends
// through a parameter of the type 'parm', in the signature 'sig'?
int lifePartFlows(struct FnSigNode *sig, INode *parm, uint32_t part, INode *to);

// May that part be stored in the slots 'slots' of 'to', a use of a struct
// declaring lifetimes: in any one of them ('all' 0), or in every one
int lifePartFlowsSlots(struct FnSigNode *sig, INode *parm, uint32_t part, INode *to, uint32_t slots, int all);

// Are the slots of the struct 'pointee' uses named apart in 'sig' -- each by
// its own name, related by no order beyond the struct's own -- so that a call
// cannot move a borrow from one slot to another the struct's order does not
// allow? An unannotated signature names none apart.
int lifeSlotsApart(struct FnSigNode *sig, INode *pointee);

// The slots a struct's field holds, as a mask; and the slots whose borrows a
// read of places holding 'slots' may carry: those, and every slot its struct's
// order lets borrows into them from
uint32_t lifeFieldSlots(struct StructNode *strnode, struct FieldDclNode *field);
uint32_t lifeSlotsReach(struct StructNode *strnode, uint32_t slots);

// The one slot a field holds, as a loan's tag (slot + 1), or 0 for a field
// holding none or several
uint32_t lifeFieldTag(struct StructNode *strnode, struct FieldDclNode *field);

// What a store through a value of this type lands in: what a borrowed
// reference points at, or else the value itself
INode *lifePointee(INode *type);

// Is this a borrowed reference type whose lifetime is written ''static'?
int lifeIsStatic(INode *type);

// Is ''static' named inside a parameter's type, anywhere but on the parameter's
// own reference? Nothing checks there that a caller's borrow is global.
int lifeParmStaticInside(INode *parmtype);

// Settle a struct's declared lifetimes once its body is parsed: a name its
// fields write that it does not declare is refused where written, but for the
// one name of a struct declaring none, which it takes as declared
void lifeStructDeclare(struct StructNode *strnode);

// Check a struct's fields once they are type checked: every borrow a struct
// declaring lifetimes holds is of one of them, never the unnamed lifetime
void lifeStructCheck(struct StructNode *strnode);

// Check a use naming lifetimes, once it is resolved: the struct must declare
// as many as it names
void lifeUseCheck(struct NameUseNode *use);

// A generic instance's use: keep the type arguments it is given where one
// holds a borrow, and the lifetimes the generic's own use named
void lifeUseInstance(struct NameUseNode *instuse, INode *genuse, Nodes *typeargs);

// Check a signature's lifetimes once its types are: mark it as naming
// lifetimes where one of its types does ('Self' of a struct declaring them
// too), refuse a 'where' clause naming a lifetime none of its types does, and
// add to its order what its parameters' and result's types imply: what the
// structs they use order, and that what a borrow points at outlasts it
void lifeSigCheck(struct FnSigNode *sig);

// Do two signatures promise the same about lifetimes? Each part of each
// parameter must flow to the result in both or in neither, a parameter be
// ''static' in both or in neither, and each part flow to what each writable
// borrowed parameter points at in both or in neither, its slots apart in both
// or in neither: those are all a call is checked against. Where their
// parameters lend different parts (a trait's 'Self' and a struct's declaring
// lifetimes), each parameter is compared whole.
int lifeSigsAgree(struct FnSigNode *a, struct FnSigNode *b);

// Does the implementation 'impl' promise at least what the requirement 'req'
// does, so that a call checked against 'req' is safe with 'impl'? Each part
// may flow to the result, or be stored through a writable parameter, only
// where it may in 'req'; a parameter is ''static' only where 'req''s is; a
// struct's slots are named apart wherever they are in 'req'. As
// lifeSigsAgree, a parameter lending different parts in each is compared whole.
int lifeSigMeets(struct FnSigNode *impl, struct FnSigNode *req);

// Spell what a signature promises about lifetimes into a type's symbol name
// (nameType), where it differs from what the signature promises unannotated:
// 'G' (where v0 puts a signature's lifetimes), a digit per promise, '_'. Two
// signatures that agree spell alike.
char *lifeSigSpell(char *bufp, struct FnSigNode *sig);

// *********************
// Lifetime bounds
// *********************
//
// A bound, '+ 'a', is said of a type whose insides are unknown: any borrow
// inside lives at least as long as ''a' (Rust's 'T: 'a' and 'dyn Trait + 'a').
//
// On a generic function's type parameter, '[T + 'a]', '[T Trait + 'a]' or
// 'where T + 'a', it is a pair in the signature's order, ''+T' >= ''a', where
// ''+T' (lifeBoundName, never written) names every borrow T's argument holds:
// an instance is made from its arguments with a bounded parameter's renamed
// to that one name (lifeRenamed) rather than erased, so its body may store a
// T where a ''a' borrow is held, or return one as ''a', and a call carries an
// argument's loans wherever ''a' flows, as for any order. ''+T' >= ''static'
// makes what a parameter lends through it global (lifePartStatic): the
// parameter holds no caller loan there, and a call is handed only a global
// borrow for it. An instance is still one per type argument, never per
// lifetime: the renaming is the parameter's, not the use's.
//
// On a virtual reference, '&<Trait + 'a' (RefNode.bound), it says what the
// referenced value's borrows outlive: the type holds ''a' as well as its own
// lifetime, so what it is read back out of carries what ''a' does, and a
// value coerced to it must hold no borrow not known to last ''a' (by band,
// by the order), which the loan walk checks where it is returned or stored
// through a parameter; a parameter that is a virtual reference already
// vouches by its own bound, or with none its own lifetime
// (lifeVirtOutlives). A parameter '&<Trait + 'static' takes only a value
// holding global borrows.
//
// An owning virtual reference, 'So[Trait]', takes no bound: it names no
// lifetime and carries no borrow, so it is bounded by ''static' (Rust's
// 'Box<dyn Trait>'), and a value made one, wherever the conversion is, may
// hold only global borrows (loanNotStaticIn, the loan walk's conversion).

// Set once the parser has read a bound of ''static': until then no
// signature bounds a lifetime by it (FnSigNode.lifestatic) and no call need
// look
extern int lifeStaticBoundSeen;

// Set once the parser has read a virtual reference's bound: until then no
// value is stored or returned as one (pwBoundHolds)
extern int lifeVirtBoundSeen;

// The name a bounded type parameter's borrows take in an instance: ''+T'
Name *lifeBoundName(Name *tparm);

// Is this such a name? The type parameter it is for, or NULL
Name *lifeBoundParm(Name *name);

// A copy of a type with every lifetime it names but an invariant one renamed
// 'to', and every borrow it holds unnamed held as 'to': a bounded parameter's
// argument, as its instance is made from it
INode *lifeRenamed(INode *type, Name *to);

// Does the generic function 'generic' bound its type parameter 'tparm'?
int lifeParmBounded(INode *generic, Name *tparm);

// Check, at name resolution, that every bound a function's order holds is on
// one of its own type parameters; a bound on anything else is refused and
// dropped from the order
void lifeBoundsNameRes(struct FnDclNode *fndcl, INode *owner);

// Is the part 'part' of what a caller lends through a parameter of the type
// 'parm' global by the signature 'sig''s order: every lifetime it holds
// bounded by ''static'?
int lifePartStatic(struct FnSigNode *sig, INode *parm, uint32_t part);

// The type parameter whose ''static' bound makes some part of a parameter of
// this type global, or NULL
Name *lifeStaticBoundOf(struct FnSigNode *sig, INode *parm);

// Is this a virtual reference type with a bound? The bound, or NULL
Name *lifeVirtBound(INode *type);

// Does what a virtual reference of this type points at hold only borrows
// lasting 'bound', by the signature's order: is its own bound, or, with none,
// its own lifetime, at least as long?
int lifeVirtOutlives(struct FnSigNode *sig, INode *vreftype, Name *bound);

// May a borrow the caller lent through the part 'part' of a parameter of the
// type 'parm' be held where only borrows lasting at least 'bound' are, by the
// signature's order?
int lifePartOutlives(struct FnSigNode *sig, INode *parm, uint32_t part, Name *bound);

// *********************
// Invariant lifetimes: brands
// *********************
//
// An invariant lifetime, ''=a', is equal only to itself. It has no order with
// any other lifetime and bounds nothing by scope: a value of one goes
// anywhere. A reference of one is a KEY ('&'=a T'), which reaches nothing on
// its own; only an arena of the same lifetime (the lock) reaches through it,
// 'arena[key]'. Inside a function each invariant lifetime is a BRAND: a name
// its signature (or 'Self') writes, or one MINTED by a call whose result
// names an invariant lifetime none of its parameters does, a fresh name per
// call site ('''=a#3''). A brand is compared by identity, nothing else (a
// 'where' clause may equate two of the signature's): never ordered, never
// solved for. Brands live in types, so every place a value meets a type --
// an argument, a store, a return, a branch joining others -- compares them
// (lifeBrandsCoerce): a call binds its callee's names to the brands its
// arguments carry and substitutes them in its result (lifeBrandSubst), and
// everything else wants the same brands it is given. A brand minted inside
// a loop names that pass's arena: no value of an outer variable's type can
// hold it, and a 'break' may not carry it out (lifeBrandBreaks).
//
// For the loan walk and the scope numbers a key is no borrow: it holds no
// lifetime that ends (lifeSetAdd skips it, itypeCarriesBorrow answers no).
// compiler/c/doc/phases/type-check-reasoning.md, "Invariant lifetimes:
// brands", is the note.

// Set once the lexer has read an invariant lifetime: until then no type
// holds one and no check need look
extern int lifeInvariantSeen;

// Is this the name of an invariant lifetime?
int lifeIsInvariant(Name *name);

// Is this type a key: a borrowed reference of an invariant lifetime?
int lifeIsKey(INode *type);

// Refuse, at 'at', reaching through a key of this type: only its arena may
void lifeKeyAccessError(INode *at, INode *keytype);

// One call's or construction's binding of the invariant names its callee's
// (or struct's) types write to the brands the caller's values carry
typedef struct LifeBind LifeBind;

// Begin binding the invariant names of 'sig' (a callee's signature) or, with
// 'sig' NULL, of a struct's own or an instance's types, at a call or a
// construction; every value coerced until lifeBindEnd binds them. Answers the
// binding, already in force.
LifeBind *lifeBindBegin(struct FnSigNode *sig);

// End it: the coercions after it compare brands as the function's own again
void lifeBindEnd(LifeBind *bind);

// Bind a struct's own invariant names (or an instance's ''=1'...) to the brands
// a use of it writes, 'None[&'=a T]', before its construction's fields do
void lifeBindUse(LifeBind *bind, INode *strnode, INode *use, INode *at);

// Bind a generic function instance's brands, named by place in 'instargs',
// to those its type arguments as written carry
void lifeBindArgs(LifeBind *bind, Nodes *instargs, Nodes *written, INode *at);

// Hold the callee's 'where '=a == '=b' to the brands bound: each pair one brand
void lifeBindClose(LifeBind *bind, INode *at);

// A copy of 'type' with each invariant name the binding knows replaced by the
// brand bound to it; a name it does not know is minted fresh at 'mintat', or,
// with 'mintat' NULL, becomes the unknown brand. 'type' itself where nothing
// changes.
INode *lifeBrandSubst(INode *type, LifeBind *bind, INode *mintat);

// Does a value of 'fromtype' carry the brands 'totype' wants? Under a
// binding, a name of the callee's is bound at its first meeting and compared
// after; otherwise a name of the current function's parameters is compared
// by identity, and one only its result (or a body's type) writes is bound
// once for the whole function. A brand lost or gained is a mismatch.
// Reports, at 'at', and answers 0 on a mismatch.
int lifeBrandsCoerce(INode *fromtype, INode *totype, INode *at);

// The type of a field 'fldtype' read from a value of 'objtype': the struct's
// own invariant names replaced by the brands the use names
INode *lifeBrandField(INode *objtype, INode *fldtype, INode *at);

// The current function's brand state, saved while another is checked
typedef struct LifeBrandSave {
    void *fn;
    LifeBind *bind;
    void *loop;
} LifeBrandSave;
void lifeBrandFnBegin(struct FnSigNode *sig, LifeBrandSave *save);
void lifeBrandFnEnd(LifeBrandSave *save);

// A loop's body is being checked: a brand minted in it is its pass's
void lifeBrandLoopEnter(INode *loop);
void lifeBrandLoopExit();

// Refuse a 'break' out of 'blk' whose value carries a brand a loop inside
// 'blk' minted: that pass's arena is gone once the pass is
void lifeBrandBreaks(INode *blk, Nodes *breaks);

// A generic's type arguments, erased (lifeErased), name their brands by
// their order, ''=1', ''=2': one instance serves every use whose brands fall
// alike, and its calls bind those names afresh (lifeBindBegin)
void lifeCanonBrands(Nodes *args);

// Do two types carry the very same brands, name for name?
int lifeBrandsEqual(INode *a, INode *b);

// Is every invariant lifetime a body's type writes one the function's
// signature names? Reports at 'at' those it does not, and answers 0.
int lifeBrandKnown(INode *type, INode *at);

// Refuse, at 'at', a type giving a key to a value that holds a borrow: what
// a key names lives in its arena, which outlives every scope. Answers 1 where
// it refused.
int lifeKeyBorrow(INode *type, INode *at);

// Does a signature name an invariant lifetime anywhere in its types? Does a type?
int lifeSigHasBrands(struct FnSigNode *sig);
int lifeTypeHasBrands(INode *type);

#endif
