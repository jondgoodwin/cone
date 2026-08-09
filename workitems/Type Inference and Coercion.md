
Bugs:
- Mut background = Rgba[]  implicit typing does not work
- Successful coercion of Some[Variant1] to Option[VarTrait]
- Untyped integer, for example `var x u32 = 5`:
	- [https://cone.jondgoodwin.com/play/index.html?gist=e2da8485600ad7b4140e270d1b284950](https://cone.jondgoodwin.com/play/index.html?gist=e2da8485600ad7b4140e270d1b284950)
	- - [https://cone.jondgoodwin.com/play/index.html?gist=bafee1a55aa7850a68162b91c9d69083](https://cone.jondgoodwin.com/play/index.html?gist=bafee1a55aa7850a68162b91c9d69083)


### Inference & Type checking

Test for i32 -> ?i32 coercion in structs
- If samesize basest trait, with tag as 1st field, loop through derived variants to find one whose 2nd field’s type matches (but no other)
- Coerce => Build TypeLit node, putting in enum and type as constraint

Coercion of Numbers/String literals: both expMatches and expCoerces get involved to set type for numbers and to borrow for literal strings
