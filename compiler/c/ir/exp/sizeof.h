/** Handling for sizeof nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef sizeof_h
#define sizeof_h

// What a layout node answers of a type: '#size(T)', '#align(T)', or '#f.offset', a field's byte offset
typedef enum {
    SizeofSize,
    SizeofAlign,
    SizeofOffset
} SizeofWhat;

// Get size of a type
typedef struct SizeofNode {
    IExpNodeHdr;
    INode *type;
    SizeofWhat what;
    FieldDclNode *field;    // SizeofOffset: the field of 'type' whose offset this is
} SizeofNode;

// A layout fact ('#size(T)', '#f.type is Copy', ...) asked of 'type' as 'what' words it. Answered once the layout
// has settled; while it is in flight the answer would be the order the declarations are laid out in, so
// it is refused (ErrorMetaLayout), naming the cycle where there is one. Returns 1 if refused.
int sizeofLayoutRefused(INode *errnode, INode *type, const char *what);

SizeofNode *newSizeofNode();

// Clone sizeof
INode *cloneSizeofNode(CloneState *cstate, SizeofNode *node);

void sizeofPrint(SizeofNode *node);

// Name resolution of sizeof node
void sizeofNameRes(NameResState *pstate, SizeofNode *node);

// Type check sizeof node
void sizeofTypeCheck(TypeCheckState *pstate, SizeofNode *node);

#endif
