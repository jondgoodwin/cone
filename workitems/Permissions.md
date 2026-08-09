
- Have flowLoadValue do read permission checks on “lval” node types (nameuse, deref, arrindex, strfield) and move iexpGetPermFlag to flow?
- Permission:  addr-of restriction on dependent types
- Programmable Lock permissions
- Conc (concurrent) permission
- [[Managed Reference Metadata Access Prototype]]

Field permissions blog/doc Necessary for safety. Special case of uni owner and mut fields: can’t allow shared/mut here for thread safety (we want isolated data that moves, can allow shared immutable, uni). Restricts us to hierarch graphs. To solve, we want to annotate these references as belonging to a region (=lifetime!). If field perm=mut w/ uni container we permit if lifetime matches (must be specified). This allows us to do scoped arenas!
