// js_sandbox.h - Sandbox utilities for JS plugins
#pragma once

#include <quickjs.h>
#include <string>

// Configure sandbox restrictions on a JS context
// This sets memory limits, stack limits, and removes dangerous globals
void js_sandbox_apply(JSContext* ctx);

// Remove potentially dangerous globals from a JS context
// e.g., Function constructor, eval (if needed)
void js_sandbox_remove_dangerous(JSContext* ctx);
