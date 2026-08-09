# Managed Reference Metadata Access Prototype

Recorded from the unstaged working-tree changes on 2026-08-08 before backing
them out.

## Goal and rationale

The prototype explores making the hidden management metadata associated with a
managed reference available through built-in field syntax:

```cone
imm regref = rcref.region
```

A managed allocation is represented internally as a three-field LLVM struct:

```text
[ region state | permission state | referenced value ]
```

`genlallocref()` allocates and initializes that whole struct, but the normal
Cone reference value points directly at the third field (`ValueField`), hiding
the region and permission fields that precede it. The intended improvement is
to treat `.region` and `.perm` as compiler-provided pseudo-fields on managed
references. This avoids adding ordinary fields or methods to every referenced
value type and provides direct access to the metadata that belongs to the
allocation rather than to the value.

The proposed compiler path is:

1. Parse `ref.region` even though `region` is a language keyword.
2. Recognize interned `region` and `perm` member names during reference
   type-checking, before normal method lookup or automatic dereferencing.
3. Lower the recognized member to a field-access expression with the
   appropriate region or permission type.
4. During LLVM generation, recover the start of the managed allocation header
   from the reference's pointer-to-value, then address `RegionField` (index 0)
   or `PermField` (index 1).

## Existing runtime representation this relies on

The supporting representation was already present before this prototype:

- `ir/types/reference.h` defines `ManagedRefFields` as `RegionField`,
  `PermField`, and `ValueField`.
- `genllvm/genlalloc.c:genlRefTypeSetup()` builds the corresponding
  `{region, permission, value}` LLVM struct and stores its struct and pointer
  types in `RefTypeInfo`.
- `genllvm/genlalloc.c:genlallocref()` returns a pointer to `ValueField`, not a
  pointer to the beginning of the allocation.
- Borrowed references do not use this allocation header:
  `genlRefTypeSetup()` returns immediately for `BorrowRegTag`.

Consequently, metadata access cannot simply cast the normal reference pointer
to the header type. It must first move backward by the target-specific byte
offset of `ValueField`.

## Affected modules and exact working-tree changes

### Name interning

#### `src/c-compiler/ir/name.h`

Added declarations for two globally interned member names:

```c
extern Name *regionName;
extern Name *permName;
```

The comments currently say `"regref"` and `"permref"`, which do not match the
actual intended spellings.

#### `src/c-compiler/ir/name.c`

Added storage for:

```c
Name *regionName;
Name *permName;
```

#### `src/c-compiler/ir/nametbl.c`

Initialized the names in `nametblInit()`:

```c
regionName = nametblFind("region", 6);
permName = nametblFind("perm", 7);
```

The `perm` length is incorrect and must be `4`, not `7`.

### Parser

#### `src/c-compiler/parser/parseexpr.c`

Changed dot-member parsing from accepting only `IdentToken` to also accepting
`RegionToken`:

```diff
-    if (lexIsToken(IdentToken)) {
+    if (lexIsToken(IdentToken) || lexIsToken(RegionToken)) {
```

This permits `rcref.region`, since `region` is tokenized as a keyword. The
prototype did not add analogous handling for any case where `perm` might be
tokenized as `PermToken`.

### Type checking and lowering

#### `src/c-compiler/ir/exp/fncall.c`

Clarified one dispatch comment from "Types" to "Nominal types", then added an
early check in the `RefTag` branch:

```c
if (node->methfld && node->methfld->tag == MbrNameUseTag) {
    Name *methname = ((NameUseNode *)node->methfld)->namesym;
    if (methname == regionName || methname == permName) {
        int x = 0;
    }
}
```

This is only a breakpoint/placeholder. It recognizes the names but does not:

- set `node->tag` to `FldAccessTag`;
- set `node->vtype` to the reference's region or permission type;
- decide whether the result is a value or borrowable lvalue;
- stop normal reference method lookup and automatic dereferencing; or
- reject borrowed references, which have no managed allocation header.

The special case exists only in the `RefTag` branch, while the code-generation
prototype also mentions `ArrayRefTag` and `VirtRefTag`. That scope mismatch
must be resolved explicitly.

### LLVM expression generation

#### `src/c-compiler/genllvm/genlexpr.c`

Added a special case in `FldAccessTag` generation for `.region` and `.perm` on
`RefTag`, `ArrayRefTag`, or `VirtRefTag`. Its intended operation is:

```text
reference-to-value
    -> byte pointer
    -> move backward to the allocation header
    -> cast to RefTypeInfo.ptrstructype
    -> GEP field 0 or 1
```

The inserted code currently:

- creates `refU8` from `genlAddr(gen, fncall->objfn)`;
- attempts to cast to `reftype->typeinfo->ptrstructype`;
- selects struct field 0 for `region` or 1 for `perm`; and
- returns the resulting field pointer.

It is not compilable or functional in its present form:

- `refToValue` is undefined;
- `reftype` is undefined;
- `refU8` and `ptrstructype` are calculated but not used;
- no subtraction of the `ValueField` offset is implemented;
- `refToRegion` is only a placeholder;
- array references require extraction of the data pointer from their fat
  pointer;
- virtual references require extraction of their object/data pointer;
- borrowed references must not use managed-header recovery; and
- the result's load/borrow behavior has not been reconciled with normal
  `FldAccessTag` generation.

The field constants from `ManagedRefFields` should be used instead of literal
indices.

### Test probe

#### `test/test.cone`

Added one compile probe to `rctest()`:

```cone
imm regref = rcref.region
```

This tests parsing and inferred access to the region metadata of an
`+rc-mut u32` reference. There is no `.perm` case, no type/value assertion, and
no coverage for arrays, virtual references, borrowed references, or mutation.

## Suggested completion approach

1. Correct the interned-name declarations/comments and change
   `nametblFind("perm", 7)` to `nametblFind("perm", 4)`.
2. Decide the supported set. The safest first implementation is managed
   allocated `RefTag` values only. Add array and virtual references only after
   defining how their fat-pointer data component maps to the same header.
3. In `fnCallTypeCheck()`, intercept `.region` and `.perm` before pointer-method
   lookup. Verify that the reference has a managed, non-borrowed region; assign
   `node->vtype` from `RefNode.region` or `RefNode.perm`; set
   `node->tag = FldAccessTag`; and return from the `RefTag` branch.
4. Define whether these pseudo-fields produce copied metadata values or lvalues
   that may be borrowed. Preserve permission safety: exposing `.perm` must not
   allow arbitrary mutation of compiler/runtime management state.
5. In LLVM generation, obtain the actual pointer-to-value expression. Compute
   the ABI offset of `ValueField` from `RefTypeInfo.structype`, subtract that
   offset using an `i8*`, cast the resulting header pointer to
   `ptrstructype`, and use `RegionField` or `PermField` with
   `LLVMBuildStructGEP`.
6. Load the field for ordinary value access, or return its address only when
   field-access borrowing semantics require an address.
7. Add positive tests for both pseudo-fields and negative tests for unsupported
   borrowed/reference forms. Verify generated behavior for a region with
   runtime state and a permission with runtime state, since empty marker types
   can conceal incorrect addressing.

## Incidental unstaged changes not part of this design

These working-tree modifications were present at the same time but are
independent of managed-reference metadata access:

- `.gitignore`: added `.vs/` to ignore Visual Studio caches.
- `Cone.vcxproj`: changed the Windows SDK from `10.0.17134.0` to `10.0` and
  upgraded Debug and Release toolsets from `v141` to `v143`.
