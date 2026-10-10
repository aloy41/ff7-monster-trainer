// Minimal, verified code patching. Every patch checks the bytes it replaces first, so a mismatched exe
// (or another mod already hooking the same spot) is reported instead of silently corrupting the game.
#pragma once
#include <stdint.h>

namespace patch {

// Redirect the E8 call at `site` so that, after the original callee returns, `after(eax)` runs.
// The callee keeps its exact stack layout (we pop our return address before calling it).
// `expected_target` = the callee we expect the call to point at today; 0 = accept whatever is there
// (chaining onto a hook another mod installed first). Returns false (and logs why) on mismatch.
using AfterFn = void(__cdecl*)(uint32_t eax);
bool hook_call_after(uintptr_t site, uintptr_t expected_target, AfterFn after, const char* name);

// Same, but after(eax, args) also gets a pointer to the call's stack arguments (cdecl: args[0] = first argument).
using AfterArgsFn = void(__cdecl*)(uint32_t eax, const uint32_t* args);
bool hook_call_after_args(uintptr_t site, uintptr_t expected_target, AfterArgsFn after, const char* name);

// Redirect the E8 call at `site` so that `before()` runs first (it may change game globals), then the
// original callee runs as normal.
using BeforeFn = void(__cdecl*)();
bool hook_call_before(uintptr_t site, uintptr_t expected_target, BeforeFn before, const char* name);

// Like hook_call_before, but before(args) can read and REWRITE the call's stack arguments before the callee runs.
using BeforeArgsFn = void(__cdecl*)(uint32_t* args);
bool hook_call_before_args(uintptr_t site, uintptr_t expected_target, BeforeArgsFn before, const char* name);

// Read the current target of an E8/E9 rel32 at `site`, 0 if it isn't one.
uintptr_t call_target(uintptr_t site);

}  // namespace patch
