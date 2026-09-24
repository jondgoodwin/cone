/** Handling for cast nodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef cast_h
#define cast_h

// Cast to another type
typedef struct CastNode {
    IExpNodeHdr;
    INode *exp;
    INode *typ;
} CastNode;

struct NameUseNode;

#define FlagConvert 0x8000  // Cast should convert instead of re-cast (default)
// The conversion a bound pattern desugars to shares its type node with the 'is'
// test before it (parseBoundMatch), and that test name resolves the node for both.
#define FlagMatchBind 0x0001  // Cast: binds a matched value; its type node is the 'is' test's

// Create node for recasting to a new type without conversion
CastNode *newRecastNode(INode *exp, INode *type);

// Create node for converting exp to a new type
CastNode *newConvCastNode(INode *exp, INode *type);

// Clone cast
INode *cloneCastNode(CloneState *cstate, CastNode *node);

// Create a new cast node
CastNode *newIsNode(INode *exp, INode *type);

// The name at the root of a pattern's type: 'Circle' in 'Circle', '&Circle',
// 'Some[i32]' or '&Some[i32]', or NULL for any other shape, a path among them.
// 'hasargs' (may be NULL) is set when the name was written with type arguments.
struct NameUseNode *castPatternName(INode *typ, int *hasargs);

// Mark a pattern's bare root name, for the parser: it is looked up in the
// matched value's enum before it is looked up lexically (FlagPattern)
void castPatternMark(INode *typ);

// Is this pattern's root name still waiting for castPatternBind? Its type
// cannot be asked of it until it is bound.
int castPatternPending(INode *typ);

void castPrint(CastNode *node);

// Name resolution of cast node
void castNameRes(NameResState *pstate, CastNode *node);

// Answer whether a value of fromtype may be converted to Bool
int castConvertsToBool(INode *fromtype);

// Type check cast node:
// - reinterpret cast types must be same size
// - Ensure type can be safely converted to target type
void castTypeCheck(TypeCheckState *pstate, CastNode *node);

// Analyze type comparison (is) node
void castIsTypeCheck(TypeCheckState *pstate, CastNode *node);

#endif
