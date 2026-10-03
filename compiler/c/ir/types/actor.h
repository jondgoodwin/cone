/** Actors: what an 'actor' declaration became
 *
 * An actor is no node of its own. The parser turns 'actor Name { ... }' into
 * ordinary declarations (parser/parseactor.c): the state, a struct of its
 * fields and methods; the message enum; the handle, the struct the actor's
 * name names, whose methods send; and the dispatch function. What the rest of
 * the compiler must still know of the actor as one thing is kept here: which
 * arguments cross to the thread the actor runs on, which the thread check asks
 * of once type check is done, and which handle stands for which state, so that
 * a use of the state through the handle is reported as private state.
 *
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef actor_h
#define actor_h

// Record an actor the parser generated: its handle, its state, and every
// parameter whose argument crosses to the actor -- each message's and each
// initializer's, 'self' aside -- as the state's own VarDclNodes, where the
// author wrote them
void actorRegister(StructNode *handle, StructNode *state, Nodes *crossing);

// Refuse each crossing parameter whose type is not Sendable, at the parameter.
// Called once, when type check has finished and every type is laid out
void actorCheckAll();

// The member of an actor's state that 'name' names, where 'type' is the
// actor's handle and the handle has no such member of its own, or NULL. '*state'
// is set to the state. A use of it is a use of the actor's private state
INode *actorStateMember(INode *type, Name *name, StructNode **state);

#endif
