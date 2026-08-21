
Array
- Auto-conversion of array to arrayref (handle strings/size issue)
- Handle single-element initialization, value and closure
- Zero terminated strings, mostly a string is a ref to the string

- “”z zero-terminated strings (fix documentation)
- Multi-level array literals (including strings)
- Handle multiline array and string literals
- Array non-literal literals (w/ variables), should they return an array reference?
- Arrays in array literal

Slice creation and member support

## Rewriting a fill literal into a loop

A fill literal `[n; value]` evaluates one expression and stores the result into
every element. Flow analysis now applies the ownership rules to it as far as a
constant can carry them: a move value may not be filled at all, since it has one
owner and cannot have n, and a counted reference raises the count by n — or by
n-1 when the value is a temporary handing over the reference it was born with.
That amount lives in the injected alias node as a compile-time constant, so a
count known only at run time is refused rather than counted wrongly.

**The general fix is to rewrite a fill literal into a loop that constructs the
array's elements one at a time, and to do it well before the generation phase.**
Two things need that, and neither can be reached from where the work sits now.
A run-time element count needs a run-time count adjustment, which a constant
`aliasamt` cannot express — this is the "Array non-literal literals (w/
variables)" bullet above, seen from the ownership side. And a generator, an
element expression yielding a different value per element, has no meaning at all
while the expression is evaluated once outside the array. Doing the rewrite early
is what makes both ordinary: the loop body goes through normal flow analysis and
aliasing like any other code, instead of code generation special-casing a fill
and every ownership rule having to be restated for it.

**One semantic question comes with the rewrite.** Today `[3; f()]` calls `f` once
and stores the result three times. A loop would make it three calls unless the
rewrite deliberately hoists the expression out, and which of those is meant is a
language decision rather than an implementation detail. It is the same question
as "Handle single-element initialization, value and closure" above, asked about a
call instead of a closure.

Note that none of this is observable yet for a value that is not a compile-time
constant, in either literal form: `genlExpr` builds an array literal with
`LLVMConstArray`, so an element that is the result of an instruction produces a
constant array with instruction operands and the module fails verification.
`array-nonconst-literal` records it, and `region-fill-count` records that a
counted fill cannot be asserted against generated code until it is fixed.

