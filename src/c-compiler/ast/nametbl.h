/** Hashed name table
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef nametbl_h
#define nametbl_h

#include "../ast/ast.h"

#include <stdlib.h>
#include <stddef.h>

// Name is an interned symbol, unique by its collection of characters (<=255)
// A name can be hashed into the global name table or a particular node's namespace.
// The struct for a name is an unmovable allocated block in memory
typedef struct Name {
	NamedAstNode *node;	    // AST node currently assigned to name
	size_t hash;			// Name's computed hash
	unsigned char namesz;	// Number of characters in the name (<=255)
	char namestr;	        // First byte of name's string (the rest follows)
} Name;

// The Global Name Table holds a context-spacific collection of names.
// - Parse uses it to resolve:
//   - reserved keywords
//   - permission names that begin a declaration of a variable.
//   - allocator names as part of a reference type
// - Name resolution uses it (see "hook" functions) to resolve:
//   - NameUse nodes to the name declaration node that they refer to

// Global Name Table configuration variables
size_t gNameTblInitSize;	          	// Initial maximum number of unique names (must be power of 2)
unsigned int gNameTblUtil;	// % utilization that triggers doubling of table

// Allocate and initialize the global name table
void nametblInit();

// Get pointer to the Name struct for the name's string in the global name table 
// For an unknown name, it allocates memory for the string and adds it to name table.
Name *nametblFind(char *strp, size_t strl);

// Return how many bytes have been allocated for global name table but not yet used
size_t nametblUnused();

// A namespace entry
typedef struct NameNode {
	Name *name;
	NamedAstNode *node;
} NameNode;

// Namespace metadata
typedef struct Namespace {
	size_t avail;
	size_t used;
	NameNode *namenodes;
} Namespace;

// The global name hook functions help with the name resolution pass.
// Whenever we enter a namespace context, the context's names are temporarily
// added to the global name table. This way the lookup of a NameUse node
// will find the innermost scope's node that declares this name.
// Sometimes all of the namespace's names are added right away.
// However, a block's local variables are added as encountered.
// When the context ends, its names are unhooked, revealing the ones there before.
void nametblHook(Namespace2 *namespace, NamedAstNode *name, Name *namesym);
void nametblHookAll(Namespace2 *namespace, Inodes *inodes);
void nametblUnhookAll(Namespace2 *namespace);

void namespaceInit(Namespace *ns, size_t avail);
NamedAstNode *namespaceFind(Namespace *ns, Name *name);
void namespaceSet(Namespace *ns, Name *name, NamedAstNode *node);

#define namespaceFor(ns) for (size_t __i = 0; __i < (ns)->avail; ++__i)
#define namespaceNextNode(ns, nodevar) \
  NamedAstNode *__namenodep = &(ns)->namenodes[__i]; \
  if (__namenodep->name == NULL) continue; \
  nodevar = __namenodep->node;

#endif