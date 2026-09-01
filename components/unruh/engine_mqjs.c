/**
 * Unruh backend: MQuickJS — TODO.
 *
 * Same engine.h contract as engine_quickjs.c. Notes for the port:
 *  - tracing GC, own allocator: point the arena at internal RAM with the
 *    board's js_heap_budget (v2 / C6 boards run without PSRAM)
 *  - ES5 subset: the App({...}) global + hook dispatch translate 1:1
 *  - bytecode loader must check the .mqb entry from the manifest
 */
#include "unruh/engine.h"
