/** '?': a Result's or an Option's value, or an early return of its failure
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef propagate_h
#define propagate_h

// 'x?' is a suffix of a term, as in Rust: '-x?' is '-(x?)', 'f()?[0]' indexes
// what 'f()?' gives. The one exception is 'await x?', which is '(await x)?'
// (parsePrefix). '?.' is held (the lexer reports it), so a member of what '?'
// gives is reached as '(x?).m'.
//
// The parser makes a block marked FlagPropagate holding 'x' in a hidden
// variable (parsePropagate), and type check finishes it (propagateLower, from
// blockTypeCheck, which has checked that variable), once it knows what 'x' is
// and what the function returns. On a Result, in a function returning a Result:
//
//   { let -held = x;
//     if -held is Ok { let -ok = [Ok]-held; -ok }
//     else if -held is Err { let -err = [Err]-held; return Err(conv(-err)) } }
//
// and on an Option, in a function returning an Option, the same with 'Some' and
// 'None', whose arm is 'return None'. It is the match the reader would write, so
// every later pass treats it as one: the arm that returns gives the 'if' no
// value (ifBlockJumps), and the last test becomes its 'else' (ifExhaustCheck).
//
// 'conv' takes the error the function is given to the error the function
// returns, in this order:
// - the same type, or one that coerces to it as any value does: as it is;
// - an 'overload from' of the function's error type that takes it,
//   'ConfigError.from(e)', selected as a call of it is;
// - the one variant of the function's error enum that holds exactly that type,
//   'Io(e)' (Cone's addition to Rust's rule). Two or more such variants and no
//   'from' is ErrorQuesAmbiguous, naming two of them;
// - otherwise ErrorQuesConvert.
//
// A value typed as one of the variants, 'Ok[i32, E](5)' or 'None[i32]', is held
// as its enum. Refused: 'x' neither a Result nor an Option (ErrorQuesOperand); a function
// returning no Result for a Result, no Option for an Option, a closure whose
// return type is not written, a generator (ErrorQuesReturn, the last at parse).
void propagateLower(TypeCheckState *pstate, BlockNode *blk);

// 'try { a? + b? }': a '?' inside leaves the block, not the function, and the
// block's value is the first failure, or its last value wrapped as the success
// ('Ok', 'Some'). The parser marks the block FlagTry (parseTry). blockTypeCheck
// calls propagateTryEnter before its statements and propagateTryFinish after
// all but the last, checking the last and the breaks against the type the
// latter answers. Enter holds the last value in a hidden variable,
//
//   try { stmts; last }   =>   { stmts; let -tryval = last; Ok(-tryval) }
//
// the 'Ok(...)' a placeholder until Finish knows the block's type (a body ending
// in ';' or a declaration succeeds with nil, 'Ok(nil)'; one ending in a jump has
// no success). A '?' met while the innermost try block being checked is of the
// function being checked (a closure's '?' is the closure's) is built as above
// with the failure's arm ending in 'break Err(conv(-err))' (or 'break None'),
// aimed at the try block. Its value is built by Finish, so the try block is a
// block with a value and breaks, as a labelled loop is.
//
// The block's type: the context's, when the block is wanted as a Result or an
// Option ('let r: Result[i32, E] = try {...}', an argument, a 'return'); else
// read off it: all '?'s on Options give 'Option[T]', all on Results with one
// error type E give 'Result[T, E]', T being the last value's type. Each '?''s
// error converts to the block's as a function's '?' converts it (overload from,
// the sole variant holding it), ErrorQuesConvert or ErrorQuesAmbiguous naming
// the try block. Refused: an Option's '?' with a Result's in one block, or one
// against a context of the other kind (ErrorTryMix, naming 'okOr' and 'ok()');
// with no context, '?'s whose errors differ, or no '?' at all, which leaves the
// kind unknown (ErrorTryType, asking for the type). 'return', 'break' and
// 'continue' inside act on the function and its loops, as anywhere.
void propagateTryEnter(TypeCheckState *pstate, BlockNode *blk, INode *expectType);
INode *propagateTryFinish(TypeCheckState *pstate, BlockNode *blk);

// A 'main' returning a Result: an error prints and the process exits with
// failure. Once a module's declarations are checked (modTypeCheck), its 'main'
// that returns a Result[T, E] is given a function the program's entry calls in
// its stead (genlEntry), private and named '-mainresult':
//
//   fn -mainresult(params) i32 {
//     let -held = main(params);
//     if -held is Err { let -err = [Err]-held; let -msg = -err.message();
//                         -mainError(-msg.view()); return 1 }
//     return 0 }
//
// where '-mainError' is conestd's 'cone_mainError(msg: &str)', declared here,
// which writes 'Error: <message>' to stderr. 'message()' gives a String (its
// view is lent) or a '&str' (passed as it is). Until core's error trait is in,
// E is asked for 'message()' by name; one without it is ErrorMainResult.
// The value an 'Ok' holds is dropped, and the exit status is 0.
void propagateMainResult(TypeCheckState *pstate, ModuleNode *mod);

// The function the entry calls in place of this 'main', or NULL when it calls
// 'main' itself
FnDclNode *propagateMainEntry(FnDclNode *mainfn);

#endif
