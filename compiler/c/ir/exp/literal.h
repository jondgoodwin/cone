/** Handling for literals
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef literal_h
#define literal_h

// Nil literal node: represents the absence of a value. Always has type "void"
typedef struct {
    IExpNodeHdr;
} NilLitNode;

// Unsigned integer literal
typedef struct {
    IExpNodeHdr;
    uint64_t uintlit;
} ULitNode;

// Float literal
typedef struct {
    IExpNodeHdr;
    double floatlit;
} FLitNode;

// String literal
typedef struct {
    IExpNodeHdr;
    char *strlit;
    uint32_t strlen;
} SLitNode;

NilLitNode *newNilLitNode();
INode *cloneNilLitNode(CloneState *cstate, NilLitNode *node);
void nilLitPrint(NilLitNode *node);

// Null literal node: the null raw pointer. Its type is the pointer type it is
// wanted as; until one is known, it is nullLitType, which nothing else has.
typedef struct {
    IExpNodeHdr;
} NullLitNode;

NullLitNode *newNullLitNode();
INode *cloneNullLitNode(CloneState *cstate, NullLitNode *node);
void nullLitPrint(NullLitNode *node);

// Is this a 'null' whose pointer type is not known yet?
int litIsUntypedNull(INode *node);

// Type check a 'null' against the type it is expected as
void nullLitTypeCheck(TypeCheckState *pstate, NullLitNode *node, INode *expectType);

// Give an untyped 'null' the type it is wanted as, which must be a raw pointer
// type: refused otherwise, or when nothing says which pointer it is. Returns 1
// when *nodep was an untyped 'null' (now typed, or reported and marked an
// error), 0 when it was not one.
int litAdoptNullType(INode **nodep, INode *totype);

// Will an untyped 'null' coerce to this type?
int litNullMatches(INode *node, INode *totype);

// Create a new fake unsigned literal node
ULitNode *newFakeULitNode(uint64_t nbr, INode *type);

ULitNode *newULitNode(uint64_t nbr, INode *type);

// Create a new unsigned literal node (after name resolution)
ULitNode *newULitNodeTC(uint64_t nbr, INode *type);

// Clone literal
INode *cloneULitNode(CloneState *cstate, ULitNode *lit);

void ulitPrint(ULitNode *node);

FLitNode *newFLitNode(double nbr, INode *type);
void flitPrint(FLitNode *node);

// Clone literal
INode *cloneFLitNode(CloneState *cstate, FLitNode *lit);

// Name resolution of lit node
void litNameRes(NameResState* pstate, IExpNode *node);

// Give an untyped integer literal the number type it is wanted as, refusing a
// value an integer type cannot hold.
// Returns 1 when *nodep is now a literal of that type, 0 otherwise.
int litAdoptNumberType(INode **nodep, INode *totype);

// Is this a character literal, as written, that stands for a u8 where one is
// wanted: a char whose value is ASCII (below 128)? Answers for a type wanted of
// u8 only. litAdoptCharAsByte gives the literal that type, returning 1 when
// *nodep is now a u8 literal, 0 otherwise.
int litCharMatchesByte(INode *node, INode *totypedcl);
int litAdoptCharAsByte(INode **nodep, INode *totype);

// An ASCII character literal that is a binary operator's receiver, beside a u8
// argument, is that byte. Returns 1 when the literal was retyped.
int litAdoptCharBesideByte(INode **objp, Nodes *args);

// Is this a character literal, as written, beyond ASCII, wanted as a u8? The
// literal is a code point and a byte is not one: refused, saying so
int litCharRefusedAsByte(INode *node, INode *totypedcl);

// Widen a float literal to a wider float type at compile time, keeping it a
// literal. Returns 1 when *nodep is now a literal of that type, 0 otherwise.
int litWidenFloat(INode **nodep, INode *totype);

// Fold a use of a named constant into a literal of the wider number type it
// reaches. Returns 1 when *nodep is now a literal of that type, 0 otherwise.
int litWidenConst(INode **nodep, INode *totype);

// Refuse an untyped integer literal, reached by generation, whose value does
// not fit the i32 it defaulted to
void litCheckDefaultRange(ULitNode *lit);

// Type check lit node
void litTypeCheck(TypeCheckState* pstate, INode **nodep, INode *expectType);

SLitNode *newSLitNode(char *str, uint32_t strlen);

// Clone literal
INode *cloneSLitNode(SLitNode *lit);

void slitPrint(SLitNode *node);

// Type check string literal node
void slitTypeCheck(TypeCheckState *pstate, SLitNode *node);

// Take a literal as the array of bytes it holds, which a written borrow of it
// is a reference to
void slitAsArray(SLitNode *node);

// Would a string literal be taken, where this type is wanted, as the byte
// array it fills or the owner of 'str' it is copied into?
int slitMatches(INode *node, INode *totypedcl);

// Make it so, returning 1 when *nodep now has the wanted type
int slitCoerce(INode **nodep, INode *totypedcl);

int litIsLiteral(INode* node);

// Fold a type-checked value where a constant is required -- an expression of
// number literals and named constants under the built-in operators,
// comparisons, 'not', 'and', 'or', conversions and reinterpretations -- into
// the literal it computes. Returns 1 when the value is now a constant, or when
// folding it reported why part of it has none; 0 when it is not a constant and
// nothing was reported, which the caller reports.
int litFoldConst(INode **nodep);

#endif
