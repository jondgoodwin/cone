# HANDOVER 5: lane 249 (12 Nov 2026). STOPPED: ruling (a) as worded does not clear the wall. Read HANDOVER4 first.

## What was tried (working tree reverted to HEAD; nothing of it is committed)
Ruling (a) built in actorhttp: `SrvRec.drv: Option[Uni[httpconn.Driver]]`, `SrvRec.h3: Option[Uni[Http3]]`, `ClientRec.client: Option[Uni[Client]]`,
private `takeDrv/putDrv/takeH3/putH3/takeClient/putClient`, and one private wrapper per call (`drvRespond`, `h3SendData`, `cliCancel`, ...: take, call on the local, put back, written once).
Results:
- Every service.cone site that called `self.srvs.borrowMutAt(k).unwrap().drv.x(lp)` or `.h3.x` directly is fixed by take/put through the cell (complete, end, sweepServers, wakeAt).
- The calls inside the (srv, slot) helpers (`srv.drvRespond(...)`) are accepted (srv is a parameter there).
- BUT the remaining ~35 E1339 are not about the receiver: they are the CALLERS of the helpers, which hand the cell borrow itself:
  `let s = sv.srvs.borrowMutAt(loc.k).unwrap(); beginResponse(s, ...)` (serverservice RespondJob/StartJob/WriteJob/FinishJob/DropExJob, requestbody ReqDropJob/ReqStopJob/ReqReleaseJob, responsesink 3 sinks), `closeServer(sv.srvs.borrowMutAt(k).unwrap(), lp)`, `unrefClient(rec, lp)`, `startStream(rec, lp, st, req)`, `rec.cliSend(...)`. The refusal is on the callee's summary ("it, or something it calls, changes the shape of one"): a callee whose body contains a virtual call, even on a local taken out, counts. So take/put of the Driver does not clear a caller that hands the borrowed SrvRec/ClientRec to a function that (transitively) reaches that call.
- Smallest repro: `C:\src\zz-lane249\r6\src\r6.cone` (build `pwsh C:\src\zz-lane249\run.ps1 r6`): `help(r)` with `r = recs.borrowMutAt(0u32).unwrap()` and `help` calling `r.virt.m()` (Uni[Tr]) is E1339 (c1) even though nothing is used after.
- What DOES compile (c2 in the same file): the whole record taken out of its cell, `boxed: GenPool[Option[Uni[Rec]]]`, `help(u)` on the local, put back. So the take/put must be of the RECORD (SrvRec, ClientRec), not of its Driver.

## What that costs (not started; design chat to rule)
Record-level take/put: `Srvs = GenPool[SrvCell]`, cell = {id, rec: Option[Uni[SrvRec]]}; every job that now does `let s = sv.srvs.borrowMutAt(loc.k).unwrap()` becomes take, a private `work(...)` method holding the old body (early returns stay inside it), put: about 20 jobs + 3 sinks on the server side, about 12 on the client side (streamservice, upload, service), plus the inner (rec, slot) conversions of streamservice/upload that lane 244 did for the server side (`streamOf`, `upFailOwn`, `Located`/UpLoc, `clientAt`). The Driver/Http3/Client Options and per-call wrappers are then unnecessary (the record is out whole). Memory trade: a server/client record is on the heap behind one Uni (few records; the hot pools Waits/Streams/Exchanges stay contiguous). While a record is out, a lookup of it by id finds an empty cell: no job re-enters (calls queue events for later pumps) but that is to be checked.
Option (b) (outer Srvs/Clients not GenPools) would avoid the box; not tested here whether the checker accepts a borrow from a plain List.

## Still undone (as HANDOVER4): streamservice, upload, service pump/send sites (ruling-1 ones are easy: copy out, re-borrow: sendBody, ConnectJob, ExchangeJob were fixed that way in the discarded attempt), h3driver tests, docs, speed pairs (speed-pair-cpu.ps1 has only aio/zstd/sort/gc kinds; copy into C:\src\zz-lane249\ and add kinds), PR body, gate-plan-1231.txt NOT written.
Line endings: actorhttp sources are CRLF; `C:\src\zz-lane249\ed.ps1` has `Ed <file> @(@(old,new),...)` which edits them safely.
