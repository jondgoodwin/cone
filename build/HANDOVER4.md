# HANDOVER 4: lane 244 (12 Nov 2026). STOPPED on ruling 3 (design question, see "The stop"). Read HANDOVER3.md, 2, 1 for the rest.

## State
- Branch genpool-contiguous, PR #1231 (draft). Compiler rebuilt in this worktree at 05:5x (it was older than the master merge); nothing else changed in the toolchain.
- Done this lane, committed: the ruling-1 helpers in serverservice.cone, requestbody.cone, responsesink.cone: `srvRespond/srvSendBuf/srvSend/srvFinish/stopUpload/srvCancel/sendContinue/answerText/inRelease/inDiscarded/inTerminal/pumpWrites/pumpSink/sinkFail/beginResponse/endBodyUnsunk/inFreeIfDone` take `(srv, slot)` and borrow inside; private `Route` + `routeOf(srv, slot)` copies (onH3, qreq, req) out; `Located` is now `{k: u32, ok: bool}` (the server's cell index and whether the exchange is live), `exLive` replaces `exOf`; every job re-borrows `sv.srvs.borrowMutAt(loc.k)`. tests/tokens.cone is on Handles.
- NOT done: streamservice.cone (`streamOf`, `upFailOwn`, `onStreamEvent`...), upload.cone (`Located`/UpLoc, `clientAt`), service.cone's pump/send sites. Those wait on the stop below, because the same wall applies.
- actorhttp does not build. 50+ E1339 remain, which the helpers do not remove (see below).

## The stop (ruling 3, reduced)
Calling any method whose body reaches a virtual call on a `Uni[Trait]` (no ShapeKeeping), with the receiver or an argument being a borrow out of a GenPool cell, is refused with E1339 even when the callee only touches a DIFFERENT pool and nothing of the cell is used after. This is why `srec.drv.x(lp)` (httpconn.Driver holds `Uni[Layer]`) and `srec.h3.x()` (`Uni[Http3]`) are refused at service.cone 223-274, and why handing the borrowed `SrvRec` to a helper is refused too (`inRelease(s, ...)`: 'is handed something that reaches a GenPool, and it, or something it calls, changes the shape of one'). Helpers cannot fix it: the argument itself is the borrow.
Repro, `C:\src\zz-genpool-lane244\r4\src\r4.cone` (build with `pwsh C:\src\zz-genpool-lane244\run.ps1 r4`, ~20 s):
```
pub trait Tr { pub fn f(self: &mut) i32; }
struct Plain { p: GenPool[u32]; fn m(self: &mut) i32 { let h = p.add(1u32); 3 } }
struct Virt  { p: GenPool[u32]; l: Uni[Tr]; fn m(self: &mut) i32 { let h = p.add(1u32); l.f() } }
struct Rec   { plain: Plain; virt: Virt; n: u32; }
pub struct S { recs: GenPool[Rec];
  pub fn a(self: &mut) i32 { recs.borrowMutAt(0u32).unwrap().plain.m() }   // accepted
  pub fn b(self: &mut) i32 { recs.borrowMutAt(0u32).unwrap().virt.m() }    // E1339
}
```
- `l: Uni[Tr + ShapeKeeping]` makes b accepted. (r4 with the marker.) But ShapeKeeping cannot hold httpconn's Layer for Plain/tls (IoBuf drop), see HANDOVER2.
- Holding the callee through a `Uni[Virt]` field does NOT help (r5): still refused.
- r2 (`C:\src\zz-genpool-lane244\r2`): `recs.borrowMutAt(0u32).unwrap().drv.complete(lp, c)` with `drv: httpconn.Driver` in the cell, refused the same way. A body with no virtual call (r1, r3: render.Visibility.clear) is accepted.
Options for design chat: (a) take the Driver and the Http3 plug-in OUT of the SrvRec/ClientRec for each call (take/put-back, as for Layer; moving a Driver costs a memcpy per call); (b) keep the Driver outside the pool (a plain List/array of records by index, the pool only for what is reached by handle); (c) a compiler change: a call through a receiver INSIDE a pool's cell, whose callee cannot reach the pool being borrowed (different element type), keeps the borrow (a compiler change, not mine to start); (d) ShapeKeeping on Layer/Http3 if IoBuf's drop can be marked.

## Left (unchanged otherwise)
tests of h3driver (needs actorhttp), docs (doc/reference, compiler/c/doc, slots row of doc/manual/library.html), the four speed pairs: NOTE `speed-pair-cpu.ps1` only knows kinds aio, zstd, sort, gc; it has no slotbench/densebench/churn/echo kind, so the pairs need a kind added or another script, PR body (HANDOVER2/3 judgement calls plus: `byToken` returns EntryId; `Located`/`routeOf`/(srv, slot) helpers private).
