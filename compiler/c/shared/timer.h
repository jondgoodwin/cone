/** Timer handling
 * @file
 *
 * This source file is part of the Cone Programming Language C compiler
 * See Copyright Notice in conec.h
*/

#ifndef timer_h
#define timer_h

#include <stdint.h>
#include <stddef.h>

enum Timers {
    LoadTimer,
    LexTimer,
    ParseTimer,
    SemTimer,
    GenTimer,
    VerifyTimer,
    OptTimer,
    CodeGenTimer,
    SetupTimer,
    FlowTimer,      // Data flow analysis: runs inside SemTimer's span, per function
    TimerCount
};

// The timer now running, so a timer started inside another's span can hand
// the time back to it when it stops
extern size_t timerCurrent;

// Whether the per-function timers run (FlowTimer). Set for -V 1 and up, so an
// ordinary compile does not pay two clock reads per function.
extern int timerFine;

// Start timing ticks for a specific timer
void timerBegin(size_t aTimer);

// Get the tick count for a timer
uint64_t timerGetTicks(size_t aTimer);

// Get a specific timer in seconds
double timerGetSecs(size_t aTimer);

// Get the summary of all timers in seconds
double timerSummary();

// Print out all timers
void timerPrint();

#endif
