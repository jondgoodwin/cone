/** Permission types
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef permission_h
#define permission_h

typedef struct NameUseNode NameUseNode;

typedef struct StructNode StructNode;

// Permission type info. A static permission has no 'lock'. A lock permission
// is a struct, not one of these; the reference a borrow through a
// lock-managed reference reads through -- the lock it holds, its guard -- has
// one of these as its permission, 'lock' the struct and 'held' which of its
// locks is held: LockHeldRead or LockHeldMut (permHeld).
typedef struct PermNode {
    INsTypeNodeHdr;
    Name *namesym;
    StructNode *lock;
    uint16_t permflags;
    uint8_t held;
} PermNode;

#define LockHeldRead 1
#define LockHeldMut 2

// Permission type flags
// - Each true flag indicates a helpful capability the permission makes possible
// - All false flags essentially deny those capabilities to the permission
#define MayRead 0x01        // If on, the contents may be read
#define MayWrite 0x02        // If on, the contents may be mutated
// If on, another live alias may be created able to read or mutate the contents. 
// If off, any new alias turns off use of the old alias for the lifetime of the new alias.
#define MayAlias 0x04        
#define MayAliasWrite 0x08    // If on, another live alias may be created able to write the contents.
#define RaceSafe 0x10        // If on, a reference may be shared with or sent to another thread.
// If on, interior references may be made within a sum type
#define MayIntRefSum 0x20
// If on, no locks are needed to read or mutate the contents. 
// If off, the permission's designated locking mechanism must be wrapped around all content access.
#define IsLockless 0x40

// Create a new permission declaration node
PermNode *newPermDclNode(Name *namesym, uint16_t flags);

// Create a new permission use node
INode *newPermUseNode(PermNode *permdcl);

// Serialize a permission node
void permPrint(PermNode *node);

// Get permission's flags
int permGetFlags(INode *perm);
   
// Are the permissions the same?
int permIsSame(INode *node1, INode *node2);

// Will 'from' permission coerce to the target?
int permMatches(INode *to, INode *from);

// Verify that permission init is correctly declared
void permInitTypeCheck(INode *perm);

// Is this permission a lock permission: a struct declaring 'LockPermission'?
int permIsLock(INode *perm);

// The lock permission's struct behind a permission: the struct itself, or the
// one a guard's permission holds a lock of; NULL for a static permission
StructNode *permLockOf(INode *perm);

// Which lock a guard's permission holds (LockHeldRead, LockHeldMut), or 0
int permHeldKind(INode *perm);

// The permission of a guard holding the lock's read or mutable lock: one node
// per lock and kind, made the first time it is asked for. A lock with no read
// pair ('acquireRead', 'releaseRead') is held mutably for a read-only borrow too.
PermNode *permHeld(StructNode *lock, int kind);

// Does this lock permission declare the read pair?
int lockPermHasRead(StructNode *lock);

// Hold a struct declaring 'LockPermission' to the shape the compiler calls
void lockPermCheck(StructNode *node);

// Refuse what was done ('read', 'write', ...) through a lock-managed
// reference without the borrow that takes its lock
void permLockRefused(INode *where, INode *perm, char *what);

#endif
