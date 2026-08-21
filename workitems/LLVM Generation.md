
LLVM Optimization bug

```
fn refdeed():

  imm ref = +[]so [10; 25.]

  ref[1] = 11.
```


Backend thoughts
- [Tcc](https://github.com/TinyCC/tinycc)
- [QBE](https://c9x.me/compile/)
- [Myrddin](https://github.com/oridb/mc)
- [LLVM back-end development](https://www.youtube.com/watch?v=AFaIP-dF-RA) and [this](https://jonathan2251.github.io/lbd/)
- [LuaJIT SSA IR intro](http://wiki.luajit.org/SSA-IR-2.0#introduction)
- [libFirm](https://pp.ipd.kit.edu/firm/index.html)



## Observations from writing `design/phases/generation.md`

Read from source, unverified. The plainly-defective ones went to [[Bugs]] and
are now fixed: `itypeMangle`'s always-true ternary, the `genlAddr`/`genlExpr`
tag mismatch on a tuple element, `genlBlock`'s asymmetric phi guard,
`genlConvert`'s uninitialized vtable pointer and its duplicated struct
conversion, the dead `genlAddr` `FnDclTag` arm, and the hardcoded debug file
name.

Two of them were worse than read: `&t.0` segfaulted, because the tag mismatch
sent the field access into a case label below it that read it as a string
literal; and the duplicated struct conversion's surviving copy allocated
mid-block, so `x into <struct>` in a long loop overflowed the stack.

Three stay here, because each needs a decision rather than a repair:

- **Two arms of `genlExpr`'s `AliasTag` case are dead.** `flowInjectAliasAmt`
  returns early unless the region is `rc` and always leaves `counts` NULL, so
  the `so` arm and the tuple `counts` arm cannot be reached. **Do not delete the
  `counts` arm** — it is exactly the machinery
  [[Copy & Alias vs. Move Semantics]] says a destructuring's alias adjustment
  needs. The `so` arm is a separate question: whether an owning reference should
  ever carry an alias node.

- **A string literal emits a fresh global per occurrence**, and constant merging
  is not in the pass list. An optimization, not a defect — worth costing against
  interning them in the name table.

- **The `FlagGenMod` condition reads like an inverted test.**
  `modname == corelibName || strcmp(filename, "stdio") ? 0 : FlagGenMod` —
  `strcmp` is non-zero for every file that is *not* `stdio`, so an ordinary
  imported module gets no `FlagGenMod` and only a `declare` is emitted. That
  matches what `test/cases/module/cases.toml` asserts today, so it may be
  deliberate while separate compilation is unfinished. Belongs with
  [[Packages and Separate Compilation]], which owns whether it should change.
