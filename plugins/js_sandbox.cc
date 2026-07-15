// js_sandbox.cc - Sandbox implementation
#include "js_sandbox.h"
#include "quickjs_helpers.h"

void js_sandbox_apply(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);

    // Set memory limit: 8 MB per plugin
    JS_SetMemoryLimit(rt, 8 * 1024 * 1024);

    // Set stack limit
    JS_SetMaxStackSize(rt, 256 * 1024);

    // Set GC threshold
    JS_SetGCThreshold(rt, 2 * 1024 * 1024);
}

void js_sandbox_remove_dangerous(JSContext* ctx) {
    // Remove eval() - we don't want plugins to dynamically execute arbitrary code
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue undef = JS_UNDEFINED;

    // Override eval with undefined
    JS_SetPropertyStr(ctx, global, "eval", JS_UNDEFINED);

    // Function constructor - also dangerous
    JS_SetPropertyStr(ctx, global, "Function", JS_UNDEFINED);

    // WebAssembly
    JS_SetPropertyStr(ctx, global, "WebAssembly", JS_UNDEFINED);

    JS_FreeValue(ctx, global);
}
