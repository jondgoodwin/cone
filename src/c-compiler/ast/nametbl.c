/** Names and Global Name Table.
 * @file
 *
 * The lexer generates symbolic, hashed tokens to speed up parsing matches (== vs. strcmp).
 * Every name is immutable and uniquely hashed based on its string characters.
 *
 * All names are hashed and stored in the global name table.
 * The name's table entry points to an allocated block that holds its current "value", computed hash and c-string.
 *
 * Dan Bernstein's djb algorithm is the hash function for strings, being fast and effective
 * The name table uses open addressing (vs. chaining) with quadratic probing (no Robin Hood).
 * The name table starts out large, but will double in size whenever it gets close to full.
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#include "nametbl.h"
#include "memory.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

// Public globals
size_t gNameTblInitSize = 16384;	// Initial maximum number of unique names (must be power of 2)
unsigned int gNameTblUtil = 80;		// % utilization that triggers doubling of table

// Private globals
Name **gNameTable = NULL;		// The name table array
size_t gNameTblAvail = 0;		// Number of allocated name table slots (power of 2)
size_t gNameTblCeil = 0;		// Ceiling that triggers table growth
size_t gNameTblUsed = 0;		// Number of name table slots used

/** String hash function (djb: Dan Bernstein)
 * Ref: http://www.cse.yorku.ca/~oz/hash.html
 * Ref: http://www.partow.net/programming/hashfunctions/#AvailableHashFunctions
 * Would this help?: hash ^= hash >> 11; // Extra randomness for long names?
 */
#define nameHashFn(hash, strp, strl) \
{ \
	char *p; \
	size_t len = strl; \
	p = strp; \
	hash = 5381; \
	while (len--) \
		hash = ((hash << 5) + hash) ^ ((size_t)*p++); \
}

/** Modulo operation that calculates primary table entry from name's hash.
 * 'size' is always a power of 2 */
#define nameHashMod(hash, size) \
	(assert(((size)&((size)-1))==0), (size_t) ((hash) & ((size)-1)) )

/** Calculate index into name table for a name using quadratic probing
 * The table's slot at index is either empty or matches the provided name/hash
 * http://stackoverflow.com/questions/2348187/moving-from-linear-probing-to-quadratic-probing-hash-collisons/2349774#2349774
 */
#define nametblFindSlot(tblp, hash, strp, strl) \
{ \
	size_t tbli, step; \
	for (tbli = nameHashMod(hash, gNameTblAvail), step = 1;; ++step) { \
		Name *slot; \
		slot = gNameTable[tbli]; \
		if (slot==NULL || (slot->hash == hash && slot->namesz == strl && strncmp(strp, &slot->namestr, strl)==0)) \
			break; \
		tbli = nameHashMod(tbli + step, gNameTblAvail); \
	} \
	tblp = &gNameTable[tbli]; \
}

/** Grow the name table, by either creating it or doubling its size */
void nametblGrow() {
	size_t oldTblAvail;
	Name **oldTable;
	size_t newTblMem;
	size_t oldslot;

	// Preserve old table info
	oldTable = gNameTable;
	oldTblAvail = gNameTblAvail;

	// Allocate and initialize new name table
	gNameTblAvail = oldTblAvail==0? gNameTblInitSize : oldTblAvail<<1;
	gNameTblCeil = (gNameTblUtil * gNameTblAvail) / 100;
	newTblMem = gNameTblAvail * sizeof(Name*);
	gNameTable = (Name**) memAllocBlk(newTblMem);
	memset(gNameTable, 0, newTblMem); // Fill with NULL pointers & 0s

	// Copy existing name slots to re-hashed positions in new table
	for (oldslot=0; oldslot < oldTblAvail; oldslot++) {
		Name **oldslotp, **newslotp;
		oldslotp = &oldTable[oldslot];
		if (*oldslotp) {
			char *strp;
			size_t strl, hash;
			Name *oldnamep = *oldslotp;
			strp = &oldnamep->namestr;
			strl = oldnamep->namesz;
			hash = oldnamep->hash;
			nametblFindSlot(newslotp, hash, strp, strl);
			*newslotp = *oldslotp;
		}
	}
	// memFreeBlk(oldTable);
}

/** Get pointer to interned Name in Global Name Table matching string. 
 * For unknown name, this allocates memory for the string and adds it to name table. */
Name *nametblFind(char *strp, size_t strl) {
	size_t hash;
	Name **slotp;

	// Hash provide string into table
	nameHashFn(hash, strp, strl);
	nametblFindSlot(slotp, hash, strp, strl);

	// If not already a name, allocate memory for string and add to table
	if (*slotp == NULL) {
		Name *newname;
		// Double table if it has gotten too full
		if (++gNameTblUsed >= gNameTblCeil)
			nametblGrow();

		// Allocate and populate name info
		*slotp = newname = memAllocBlk(sizeof(Name) + strl);
		memcpy(&newname->namestr, strp, strl);
		(&newname->namestr)[strl] = '\0';
		newname->hash = hash;
		newname->namesz = (unsigned char)strl;
		newname->node = NULL;		// Node not yet known
	}
	return *slotp;
}

// Return size of unused space for name table
size_t nametblUnused() {
	return (gNameTblAvail-gNameTblUsed)*sizeof(Name*);
}

// Initialize name table
void nametblInit() {
	nametblGrow();
}


// ************************ Name table hook *******************************

// Resolving name references to their scope-correct name definition can get complicated.
// One approach involves the equivalent of a search path, walking backwards through
// lexically-nested namespaces to find the first name that matches. This approach
// can take time, as it may involve a fair amount of linear searching.
//
// Name hooking is a performant alternative to search paths. When a namespace
// context comes into being, its names are "hooked" into the global symbol table,
// replacing the appropriate IR node with the current one for that name.
// Since all names are memoized symbols, there is no search (linear or otherwise).
// The node you want is already attached to the name. When the scope ends,
// the name's node is unhooked and replaced with the saved node for an earlier scope.
//
// Name hooking is particularly important for name resolution.
// However, it is also used during parsing for modules, who need
// to resolve permissions and allocators for correct syntactic decoding.
//
// Hooking uses a LIFO stack of hook tables that preserve the old name/node pairs
// for later restoration when unhooking. Hook tables are reused and will grow as
// needed, again for performance.

// An entry for preserving the node that was in global name table for the name
typedef struct {
    NamedAstNode *node;       // The previous node to restore on pop
    Name *name;          // The name the node was indexed as
} HookTableEntry;

typedef struct {
    HookTableEntry *hooktbl;
    uint32_t size;
    uint32_t alloc;
} HookTable;

HookTable *gHookTables = NULL;
int gHookTablePos = -1;
int gHookTableSize = 0;

// Create a new hooked context for name/node associations
void nametblHookPush() {

    ++gHookTablePos;

    // Ensure we have a large enough area for HookTable pointers
    if (gHookTableSize == 0) {
        gHookTableSize = 32;
        gHookTables = (HookTable*)memAllocBlk(gHookTableSize * sizeof(HookTable));
        memset(gHookTables, 0, gHookTableSize * sizeof(HookTable));
        gHookTablePos = 0;
    }
    else if (gHookTablePos >= gHookTableSize) {
        // Double table size, copying over old data
        HookTable *oldtable = gHookTables;
        int oldsize = gHookTableSize;
        gHookTableSize <<= 1;
        gHookTables = (HookTable*)memAllocBlk(gHookTableSize * sizeof(HookTable));
        memset(gHookTables, 0, gHookTableSize * sizeof(HookTable));
        memcpy(gHookTables, oldtable, oldsize * sizeof(HookTable));
    }

    HookTable *table = &gHookTables[gHookTablePos];

    // Allocate a new HookTable, if we don't have one allocated yet
    if (table->alloc == 0) {
        table->alloc = gHookTablePos == 0 ? 128 : 32;
        table->hooktbl = (HookTableEntry *)memAllocBlk(table->alloc * sizeof(HookTableEntry));
        memset(table->hooktbl, 0, table->alloc * sizeof(HookTableEntry));
    }
    // Let's re-use the one we have
    else
        table->size = 0;
}

// Double the size of the current hook table
void nametblHookGrow() {
    HookTable *tablemeta = &gHookTables[gHookTablePos];
    HookTableEntry *oldtable = tablemeta->hooktbl;
    int oldsize = tablemeta->alloc;
    tablemeta->alloc <<= 1;
    tablemeta->hooktbl = (HookTableEntry *)memAllocBlk(tablemeta->alloc * sizeof(HookTableEntry));
    memset(tablemeta->hooktbl, 0, tablemeta->alloc * sizeof(HookTableEntry));
    memcpy(tablemeta->hooktbl, oldtable, oldsize * sizeof(HookTableEntry));
}

// Hook the named node in the current hooktable
void nametblHookNode(NamedAstNode *node) {
    HookTable *tablemeta = &gHookTables[gHookTablePos];
    if (tablemeta->size + 1 >= tablemeta->alloc)
        nametblHookGrow();
    HookTableEntry *entry = &tablemeta->hooktbl[tablemeta->size++];
    entry->node = node;
    entry->name = node->namesym;
}

// Hook the named node using an alias into the current hooktable
void nametblHookAlias(Name *name, NamedAstNode *node) {
    HookTable *tablemeta = &gHookTables[gHookTablePos];
    if (tablemeta->size + 1 >= tablemeta->alloc)
        nametblHookGrow();
    HookTableEntry *entry = &tablemeta->hooktbl[tablemeta->size++];
    entry->node = node;
    entry->name = name;
}

// Unhook all names in current hooktable, then revert to the prior hooktable
void nametblHookPop() {
    HookTable *tablemeta = &gHookTables[gHookTablePos];
    HookTableEntry *entry = tablemeta->hooktbl;
    int cnt = tablemeta->size;
    while (cnt--) {
        entry->name->node = entry->node;
        --entry;
    }
    --gHookTablePos;
}

// Hook a node into global name table, such that its owner can withdraw it later
void nametblHook(Namespace2 *namespace, NamedAstNode *namenode, Name *name) {
	namenode->hooklink = namespace->nameslink; // Add to owner's hook list
	namespace->nameslink = (NamedAstNode*)namenode;
	namenode->prevname = name->node; // Latent unhooker
	name->node = (NamedAstNode*)namenode;
}

// Hook all names in inodes into global name table
void nametblHookAll(Namespace2 *namespace, Inodes *inodes) {
	SymNode *nodesp;
	uint32_t cnt;
	for (inodesFor(inodes, cnt, nodesp))
		nametblHook(namespace, nodesp->node, nodesp->name);
}

// Unhook all of an owner's names from global name table (LIFO)
void nametblUnhookAll(Namespace2 *namespace) {
	NamedAstNode *node = namespace->nameslink;
	while (node) {
		NamedAstNode *next = node->hooklink;
		node->namesym->node = node->prevname;
		node->prevname = NULL;
		node->hooklink = NULL;
		node = next;
	}
	namespace->nameslink = NULL;
}

// ************************ Namespace *******************************

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
	newTblMem = namespace->avail * sizeof(NameNode*);
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
	if (ns->used > ns->avail * 80 / 100)
		namespaceGrow(ns);
	NameNode *slotp;
	namespaceFindSlot(slotp, ns, name);
	slotp->name = name;
	slotp->node = node;
}
