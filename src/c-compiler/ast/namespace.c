/** Hashed named astnodes.
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "nametbl.h"
#include "namespace.h"
#include "memory.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

/** Modulo operation that calculates primary table entry from name's hash.
* 'size' is always a power of 2 */
#define nameHashMod(hash, size) \
	(assert(((size)&((size)-1))==0), (size_t) ((hash) & ((size)-1)) )

// Find the NameNode slot owned by a name
#define namespaceFindSlot(slot, ns, namep) \
{ \
	size_t tbli, step; \
	for (tbli = nameHashMod((namep)->hash, (ns)->avail), step = 1;; ++step) { \
		slot = &(ns)->namenodes[tbli]; \
		if (slot->name == NULL || slot->name == (namep)) \
			break; \
		tbli = nameHashMod(tbli + step, (ns)->avail); \
	} \
}

// Grow the namespace, by either creating it or doubling its size
void namespaceGrow(Namespace *namespace) {
	size_t oldTblAvail;
	NameNode *oldTable;
	size_t newTblMem;
	size_t oldslot;

	// Use avail for new table, otherwise double its size
	if (namespace->used == 0) {
		oldTblAvail = 0;
	}
	else {
		oldTblAvail = namespace->avail;
		namespace->avail <<= 1;
	}

	// Allocate and initialize new name table
	oldTable = namespace->namenodes;
	newTblMem = namespace->avail * sizeof(NameNode);
	namespace->namenodes = (NameNode*)memAllocBlk(newTblMem);
	memset(namespace->namenodes, 0, newTblMem);

	// Copy existing name slots to re-hashed positions in new table
	for (oldslot = 0; oldslot < oldTblAvail; oldslot++) {
		NameNode *oldslotp, *newslotp;
		oldslotp = &oldTable[oldslot];
		if (oldslotp->name) {
			namespaceFindSlot(newslotp, namespace, oldslotp->name);
			newslotp->name = oldslotp->name;
			newslotp->node = oldslotp->node;
		}
	}
	// memFreeBlk(oldTable);
}

// Initialize a namespace with a specific number of slots
void namespaceInit(Namespace *ns, size_t avail) {
	ns->used = 0;
	ns->avail = avail;
	ns->namenodes = NULL;
	namespaceGrow(ns);
}

// Return the node for a name (or NULL if none)
NamedAstNode *namespaceFind(Namespace *ns, Name *name) {
	NameNode *slotp;
	namespaceFindSlot(slotp, ns, name);
	return slotp->node;
}

// Add or change the node a name maps to
void namespaceSet(Namespace *ns, Name *name, NamedAstNode *node) {
    size_t cap = (ns->avail * 100) >> 7;
	if (ns->used > cap)
		namespaceGrow(ns);
	NameNode *slotp;
	namespaceFindSlot(slotp, ns, name);
    if (slotp->node == NULL)
        ++ns->used;
	slotp->name = name;
	slotp->node = node;
}

// Add fn/method to namespace, where multiple allowed with same name
// When multiple exist, they are mediated by a FnTupleAstNode
void namespaceAddFnTuple(Namespace *ns, NamedAstNode *fn) {
    Name *name = fn->namesym;
    NamedAstNode *foundnode = namespaceFind(ns, name);
    if (foundnode) {
        FnTupleAstNode *tuple;
        if (foundnode->asttype == FnTupleNode)
            tuple = (FnTupleAstNode *)foundnode;
        else {
            tuple = newFnTupleNode(name);
            namespaceSet(ns, name, (NamedAstNode*)tuple);
            nodesAdd(&tuple->methods, (AstNode*)foundnode);
        }
        nodesAdd(&tuple->methods, (AstNode*)fn);
    }
    else
        namespaceSet(ns, name, (NamedAstNode*)fn);

}