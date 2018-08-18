/** Header and structure for inamednodes
 *
 * Largely, the functions that work with named nodes are distributed
 * across the various namespaces:  nametbl, namespace, nodes, methnodes
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef inamed_h
#define inamed_h

#include <stdlib.h>

typedef struct INamedNode INamedNode;

// Name is an interned symbol, unique by its collection of characters (<=255)
// A name can be hashed into the global name table or a particular node's namespace.
// The struct for a name is an unmovable allocated block in memory
typedef struct Name {
    INamedNode *node;	    // Node currently assigned to name
    size_t hash;			// Name's computed hash
    unsigned char namesz;	// Number of characters in the name (<=255)
    char namestr;	        // First byte of name's string (the rest follows)
} Name;

// Named Node header, for variable and type declarations
// - namesym points to the global name table entry (holds name string)
// - owner is the namespace node this name belongs to
#define INamedNodeHdr \
	ITypedNodeHdr; \
	Name *namesym; \
	struct INamedNode *owner

// Castable structure for all named nodes
typedef struct INamedNode {
	INamedNodeHdr;
} INamedNode;

#endif