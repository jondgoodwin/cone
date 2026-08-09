
### Option

Use Option to replace nullable reference
- Inference for `None` - not having to specify type or typelit[]
- Supertype/inference for Some/None on multiple branches
- Optimize nullable references: use nullable pointer flag to create ptr, type literal, ptr compare, and pointer extraction/bind
- Remove old “nulltag” logic? (but keep null for ptrs?)

Option operators/Pattern match
- Improve pattern match for Some and None
- Flow typing. Conditional expression: For Null, nothing more. For not null, create unwrapped shadow var in block. Also works if conditional exp uses and right after
- Null testing for pointer
- ‘or’ operator. If x is not null {x.unwrap} else {def}. Confirm types match.
- ?. operator on method calls. If x {Some[x.y()]} else Null[type]. Also handle on lhs of assign
- ? force unwrap + exception: perform exception + panic


