/** Hashed named astnodes
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef namespace_h
#define namespace_h

typedef struct Name Name;

// A namespace entry
typedef struct NameNode {
	Name *name;
	INamedNode *node;
} NameNode;

// Namespace metadata
typedef struct Namespace {
	size_t avail;
	size_t used;
	NameNode *namenodes;
} Namespace;

void namespaceInit(Namespace *ns, size_t avail);
INamedNode *namespaceFind(Namespace *ns, Name *name);
void namespaceSet(Namespace *ns, Name *name, INamedNode *node);
// Add fn/method to namespace, where multiple allowed with same name
// When multiple exist, they are mediated by a FnTupleNode
void namespaceAddFnTuple(Namespace *ns, INamedNode *fn);

#define namespaceFor(ns) for (size_t __i = 0; __i < (ns)->avail; ++__i)
#define namespaceNextNode(ns, nodevar) \
  NameNode *__namenodep = &(ns)->namenodes[__i]; \
  if (__namenodep->name == NULL) continue; \
  nodevar = __namenodep->node;

#endif