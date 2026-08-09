**Types**
- Support type initializers: type.init(), complete with &mut implicit first parm
- Switch type constructor +.init to () 

**Regions:**
- Init ?
- Finalization triggered prior to free
	- Handle final on values (statement drops)
	- Final handling for union - build dropfn & activate
	- Final handling for tuple

**Modules:**
- Init
- Final

