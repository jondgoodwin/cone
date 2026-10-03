/** Handling for array types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef array_h
#define array_h

// Array node: Either an array type or an array literal
typedef struct {
    ITypeNodeHdr;
    Nodes *dimens;    // Dimensions of the array
    Nodes *elems;     // Either a list of elements, or the element type
    uint32_t *repeats; // An array's contents: how many elements each of elems fills; NULL, one each
    // An array type: how many sizes 'Array[T, n, m, ...]' was written with, on
    // the outermost node of those it lowered to; 0 for every other array type.
    // The written shape decides only what a construction's contents are
    // (contentsArrayLit): type identity, layout and naming never read it.
    // An array literal: how many levels of its type its elements are the
    // scalars of, row-major; 0 when they are its own elements.
    uint32_t nsizes;
} ArrayNode;

// Create a new array node
ArrayNode *newArrayNode();

// Clone array
INode *cloneArrayNode(CloneState *cstate, ArrayNode *node);

// Create a new array type of a specified size and element type
ArrayNode *newArrayNodeTyped(INode *lexnode, size_t size, INode *elemtype);

// Return the element type of the array type
INode *arrayElemType(INode *array);

// Return the size of the first dimension (assuming 1-dimensional array)
uint64_t arrayDim1(INode *array);

void arrayPrint(ArrayNode *node);

// Name resolution of an array literal, refusing '[n; T]' as a type
void arrayNameRes(NameResState *pstate, ArrayNode *node);

// Lower the array type 'Array[T, n, ...]', a name-resolved bracketed call on
// 'Array', into the (nested) array type node it names
void arrayTypeLower(NameResState *pstate, INode **nodep);

// A generic parameter's substituted type, an array type's written shape removed
INode *arrayTypeUnshaped(CloneState *cstate, INode *type);

// Type check an array type
void arrayTypeCheck(TypeCheckState *pstate, ArrayNode *name);

// What an array type reads from its element's layout: its size, and whether
// it moves. Part of its check, unless the array is a reference's target while
// a layout is in flight, when it waits for the element (structArrayWait).
void arrayTypeFinish(ArrayNode *node);

int arrayEqual(ArrayNode *node1, ArrayNode *node2);

// Is from-type a subtype of to-struct (we know they are not the same)
TypeCompare arrayMatches(ArrayNode *to, ArrayNode *from, SubtypeConstraint constraint);

#endif
