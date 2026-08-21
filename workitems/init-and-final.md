**Types**
- Support type initializers: type.init(), complete with &mut implicit first parm
- Switch type constructor +.init to () 
	- **The `()` path already exists in `fnCallTypeCheck` and nothing exercises
	  it.** Measured over all 122 corpus sources: the arm that takes a type name
	  called without `[`, rewrites the name to `init` and looks it up with
	  `namespaceFind`, fired **zero times**. The bracket form `Point[1., 2.]`
	  exits earlier into `typeLitTypeCheck` and is well covered; the paren form
	  is not covered at all. So this item lands on untested code rather than on
	  new code, and wants a scenario before it is moved, not after -- the same
	  "cover before you move" rule [[analysis-re-factor|Analysis re-factor]] used for the two
	  lowerings it relocated.
	- Worth knowing: that lookup is one of the places [[analysis-re-factor|Analysis re-factor]]
	  made sound. Demand guarantees the type is laid out -- mixins expanded,
	  trait methods inherited -- before `namespaceFind` asks it for `init`, so
	  whether the initializer is found no longer depends on source order. It is
	  untested, not unsafe.

**Regions:**
- Init ?
- Finalization triggered prior to free
	- Handle final on values (statement drops)
	- Final handling for union - build dropfn & activate
	- Final handling for tuple

**Modules:**
- Init
- Final

