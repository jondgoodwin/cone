/** Handling for the SIMD vector type, 'Vector[T, N]'
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef vector_h
#define vector_h

// The most lanes a vector has. A vector of N lanes is built from N values where
// a run of them is written out, and lowers to LLVM's <N x T>.
#define VectorMaxLanes 64

// A vector type: N lanes of one number type, lowered to LLVM's <N x T>. It is a
// type that supports methods: the operators and lane methods its lane type
// allows are made when the type is checked (vectorMethods), and are intrinsics
// that generation expands, as a number's are. Its mask, the 'Vector[bool, N]'
// its comparisons give, is a vector too.
typedef struct VectorNode {
    INsTypeNodeHdr;
    Nodes *dimens;    // The lane count, one ULitNode (a value parameter's use in a generic's template)
    Nodes *elems;     // The lane type, one
    struct VectorNode *mask;    // What a comparison gives; the vector itself when its lanes are bool
} VectorNode;

// Create a new vector type node
VectorNode *newVectorNode();

// Clone a vector type
INode *cloneVectorNode(CloneState *cstate, VectorNode *node);

// Serialize a vector type as it is written, 'Vector[f32, 4]'
void vectorPrint(VectorNode *node);

// Name resolution of a vector type
void vectorNameRes(NameResState *pstate, VectorNode *node);

// Lower the type 'Vector[T, N]', a name-resolved bracketed call on 'Vector',
// into the vector type node it names
void vectorTypeLower(NameResState *pstate, INode **nodep);

// Type check a vector type: its lane type and count, then its methods
void vectorTypeCheck(TypeCheckState *pstate, VectorNode *node);

// The lane type, and the lane count
INode *vectorElemType(INode *vec);
uint64_t vectorLanes(INode *vec);

// Is this the mask type, 'Vector[bool, N]'?
int vectorIsMask(INode *vec);

// The mask a comparison of this vector's lanes gives
INode *vectorMaskType(INode *vec);

// Do two vector types have the same lane type and count?
int vectorEqual(VectorNode *node1, VectorNode *node2);

// Is from-type a subtype of to-type (we know they are not the same)
TypeCompare vectorMatches(VectorNode *to, VectorNode *from, SubtypeConstraint constraint);

// A construction from one value, 'Vector[T, N].from(x)' and the broadcast of a
// scalar beside a vector: the node generation expands (genlVectorConvert)
FnCallNode *vectorNewConvert(INode *vectype, INode *arg, INode *lexnode);

// A method of a vector not found: say why in words about vectors (a mask used
// as a number, lanes of different types). Answers 1 when it reported.
int vectorMissingMember(FnCallNode *callnode, INode *vectype, Name *methsym);

// A call of a vector method that selected no candidate: say why in words about
// vectors, a lane type or count mixed with another. Answers 1 when it reported.
int vectorNoCandidate(FnCallNode *callnode, INode *vectype, Name *methsym);

// 'mask.select(a, b)': lane by lane a where the mask is true, else b.
// Type checked here because its two arguments' type is any vector of the mask's
// lane count, which no signature says. Answers 1 when it handled the call.
int vectorSelectLower(TypeCheckState *pstate, FnCallNode *callnode);

// An operator whose operands are a vector and a number: the number is
// broadcast to a vector of the lane type. Answers 1 when it made the
// operands vectors (or reported that it could not)
int vectorBroadcastOperand(TypeCheckState *pstate, FnCallNode *callnode);

// 'Vector[T, N].from(x)', x an array of the same type and size, a vector of
// the same size, or a number: the conversion, a type literal for generation.
// Answers 1 when it handled the call.
int vectorFromCall(TypeCheckState *pstate, FnCallNode **nodep);

// Index a vector: 'v[i]' is the lane, a place. Type checks the index and
// gives the node its lane type. Answers 0 when it reported.
int vectorIndex(FnCallNode *node);

#endif
