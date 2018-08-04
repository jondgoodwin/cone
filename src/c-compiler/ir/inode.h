/** The INode interface underlying all IR nodes.
*
* All IR nodes begin with header fields that specify
* - Which specific node it is, and what node groups it belongs to
* - Node-specific flags distinguishing variations
* - Lexer position info, to improve the helpfulness of error messages
*
* All nodes can be channeled through helpful functions:
* - Dispatch for the semantic passes
* - Serializing the IR nodes (for diagnostic purposes)
*
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef inode_h
#define inode_h

// All IR nodes begin with this header, mostly containing lexer data describing
// where in the source this structure came from (useful for error messages)
// - asttype contains the NodeTypes code
// - flags contains node-specific flags
// - lexer contains -> url (filepath) and -> source
// - srcp points to start of source token
// - linep points to start of line that token begins on
// - linenbr is the source file's line number, starting with 1
#define INodeHdr \
	Lexer *lexer; \
	char *srcp; \
	char *linep; \
	uint32_t linenbr; \
	uint16_t asttype; \
	uint16_t flags

// INode is a castable struct for all IR nodes.
typedef struct INode {
    INodeHdr;
} INode;

// Flags found at the top of an node's asttype
#define VoidGroup  0x0000   // Statement nodes that return no value
#define ValueGroup 0x4000   // Nodes that return a value
#define TypeGroup  0x8000   // Nodes that define or refer to a type
#define GroupMask  0xC000
#define NamedNode  0x2000   // Node that defines a named item (not nameuse)
#define MethodType 0x1000   // Type that supports methods

// Easy checks on the kind of node it is based on high-level flags
#define isValueNode(node) (((node)->asttype & GroupMask) == ValueGroup)
#define isTypeNode(node) (((node)->asttype & GroupMask) == TypeGroup)
#define isNamedNode(node) ((node)->asttype & NamedNode)
#define isMethodType(node) (isTypeNode(node) && (node)->asttype & MethodType)

// All the possible types for an node
enum NodeTypes {
	// Lexer-only nodes that are *never* found in a program's IR.
	// KeywordNode exists for name table consistency
	KeywordNode = VoidGroup,	// Keyword token (flags is the keyword's token type)

	// Untyped (Basic) nodes
	IntrinsicNode,	// Alternative to fndcl block for internal operations (e.g., add)
	ReturnNode,     // Return node
	WhileNode,		// While node
	BreakNode,		// Break node
	ContinueNode,	// Continue node

	// Name usage (we do not know what type of name it is until name resolution pass)
	NameUseTag,  	// Name use node (pre-name resolution)

    ModuleNode = VoidGroup + NamedNode,		// Program (global area)

    // Expression nodes (having value type - or sometimes nullType)
    VarNameUseTag = ValueGroup,  // Variable or Function name use node  
	MbrNameUseTag,	// Member of a type's namespace (property/method)
	ULitNode,		// Integer literal
	FLitNode,		// Float literal
	SLitNode,		// String literal
	AssignNode,		// Assignment expression
	FnCallNode,		// Function+method call or Property access
	SizeofNode,		// Sizeof a type (usize)
	CastNode,		// Cast exp to another type
	AddrNode,		// & (address of) operator
	DerefNode,		// * (pointed at) operator
	NotLogicNode,	// ! / not
	OrLogicNode,	// || / or
	AndLogicNode,	// && / and
	BlockNode,		// Block (list of statements)
	IfNode,			// if .. elif .. else statement

    // Named value node
    VarDclTag = ValueGroup + NamedNode,
    FnDclTag,

    // Unnamed type node
    TypeNameUseTag = TypeGroup, // Type name use node
    FnSigType,	// Also method, closure, behavior, co-routine, thread, ...
    VoidType,	// representing no values, e.g., no return values on a fn

    RefType = TypeGroup + NamedNode,	// Reference
    PtrType,	// Pointer

    IntNbrType = TypeGroup + NamedNode + MethodType,	// Integer
    UintNbrType,	// Unsigned integer
    FloatNbrType,	// Floating point number
    StructType,	// Also interface, trait, tuple, actor, etc.
    ArrayType,	// Also dynamic arrays? SOA?
    PermType,
    AllocType,
};

// *****************
// Node-specific flags
// *****************

// VarDclTag and FnDclTag flags
#define	FlagMethProp 0x0001	    // Method or Property (vs. static)
#define FlagExtern 0x0002		// C ABI extern (no value, no mangle)


// Allocate and initialize the INode portion of a new node
#define newNode(node, nodestruct, nodetype) {\
	node = (nodestruct*) memAllocBlk(sizeof(nodestruct)); \
	node->asttype = nodetype; \
	node->flags = 0; \
	node->lexer = lex; \
	node->srcp = lex->tokp; \
	node->linep = lex->linep; \
	node->linenbr = lex->linenbr; \
}

// Copy lexer info over to another node
#define copyNodeLex(newnode, oldnode) { \
    (newnode)->lexer = (oldnode)->lexer; \
    (newnode)->srcp = (oldnode)->srcp; \
    (newnode)->linep = (oldnode)->linep; \
    (newnode)->linenbr = (oldnode)->linenbr; \
}

// Helper functions for serializing a node
void inodePrint(char *dir, char *srcfn, INode *pgm);
void inodePrintNode(INode *node);
void inodeFprint(char *str, ...);
void inodePrintNL();
void inodePrintIndent();
void inodePrintIncr();
void inodePrintDecr();

// Dispatch a node walk for the current semantic analysis pass
// - pstate is helpful state info for node traversal
// - node is a pointer to pointer so that a node can be replaced
void inodeWalk(PassState *pstate, INode **pgm);

#endif