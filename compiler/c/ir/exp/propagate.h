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
//     else if -held is Error { let -err = [Error]-held; return Error(conv(-err)) } }
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

// A 'main' returning a Result: an error prints and the process exits with
// failure. Once a module's declarations are checked (modTypeCheck), its 'main'
// that returns a Result[T, E] is given a function the program's entry calls in
// its stead (genlEntry), private and named '-mainresult':
//
//   fn -mainresult(params) i32 {
//     let -held = main(params);
//     if -held is Error { let -err = [Error]-held; let -msg = -err.message();
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
