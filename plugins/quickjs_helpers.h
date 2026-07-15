// quickjs_helpers.h - RAII wrappers and utilities for QuickJS
#pragma once

#include <quickjs.h>
#include <string>
#include <memory>
#include <stdexcept>

// ==================== RAII Scoped JSValue ====================
// Automatically calls JS_FreeValue on destruction
class JsValue {
    JSContext* ctx_ = nullptr;
    JSValue val_ = JS_UNDEFINED;
    bool owned_ = false;

public:
    JsValue() = default;

    JsValue(JSContext* ctx, JSValue val, bool take_ownership = true)
        : ctx_(ctx), val_(val), owned_(take_ownership) {}

    ~JsValue() { free(); }

    JsValue(JsValue&& other) noexcept
        : ctx_(other.ctx_), val_(other.val_), owned_(other.owned_) {
        other.owned_ = false;
    }

    JsValue& operator=(JsValue&& other) noexcept {
        if (this != &other) {
            free();
            ctx_ = other.ctx_;
            val_ = other.val_;
            owned_ = other.owned_;
            other.owned_ = false;
        }
        return *this;
    }

    JsValue(const JsValue&) = delete;
    JsValue& operator=(const JsValue&) = delete;

    JSValue get() const { return val_; }
    JSValue release() { owned_ = false; return val_; }
    bool is_valid() const { return !JS_IsException(val_); }

    explicit operator bool() const { return is_valid(); }

    // Check types
    bool is_object() const { return JS_IsObject(val_); }
    bool is_string() const { return JS_IsString(val_); }
    bool is_number() const { return JS_IsNumber(val_); }
    bool is_bool() const { return JS_IsBool(val_); }
    bool is_null() const { return JS_IsNull(val_); }
    bool is_undefined() const { return JS_IsUndefined(val_); }
    bool is_array() const { return JS_IsArray(val_); }
    bool is_exception() const { return JS_IsException(val_); }

    // Convert to C++ types
    std::string to_string() const {
        if (!is_string() && !is_number() && !is_bool()) return "";
        JsValue str(ctx_, JS_ToString(ctx_, val_));
        const char* cstr = JS_ToCString(ctx_, str.get());
        if (!cstr) return "";
        std::string result(cstr);
        JS_FreeCString(ctx_, cstr);
        return result;
    }

    int32_t to_int32(int32_t default_val = 0) const {
        if (is_undefined() || is_null()) return default_val;
        int32_t result;
        if (JS_ToInt32(ctx_, &result, val_)) return default_val;
        return result;
    }

    int64_t to_int64(int64_t default_val = 0) const {
        if (is_undefined() || is_null()) return default_val;
        int64_t result;
        if (JS_ToInt64(ctx_, &result, val_)) return default_val;
        return result;
    }

    double to_float64(double default_val = 0.0) const {
        if (is_undefined() || is_null()) return default_val;
        double result;
        if (JS_ToFloat64(ctx_, &result, val_)) return default_val;
        return result;
    }

    bool to_bool(bool default_val = false) const {
        if (is_undefined() || is_null()) return default_val;
        int r = JS_ToBool(ctx_, val_);
        return r < 0 ? default_val : (r != 0);
    }

    // Get property as string (convenience)
    std::string get_property_str(const char* prop) const {
        JsValue v(ctx_, JS_GetPropertyStr(ctx_, val_, prop));
        if (!v.is_string()) return "";
        return v.to_string();
    }

    JSContext* ctx() const { return ctx_; }

private:
    void free() {
        if (owned_ && ctx_ && !JS_IsUndefined(val_)) {
            JS_FreeValue(ctx_, val_);
        }
        val_ = JS_UNDEFINED;
        owned_ = false;
    }
};

// ==================== RAII Scoped JSRuntime ====================
struct JsRuntimeDeleter {
    void operator()(JSRuntime* rt) const {
        if (rt) JS_FreeRuntime(rt);
    }
};
using JsRuntimePtr = std::unique_ptr<JSRuntime, JsRuntimeDeleter>;

// ==================== RAII Scoped JSContext ====================
struct JsContextDeleter {
    void operator()(JSContext* ctx) const {
        if (ctx) JS_FreeContext(ctx);
    }
};
using JsContextPtr = std::unique_ptr<JSContext, JsContextDeleter>;

// ==================== Result type for JS hook calls ====================
struct JsCheckResult {
    bool should_reject = false;
    std::string reason;
};

// ==================== Scoped C string from QuickJS ====================
class JsCString {
    JSContext* ctx_;
    const char* str_;
public:
    JsCString(JSContext* ctx, const char* str) : ctx_(ctx), str_(str) {}
    ~JsCString() { if (str_) JS_FreeCString(ctx_, str_); }
    JsCString(const JsCString&) = delete;
    JsCString& operator=(const JsCString&) = delete;
    const char* get() const { return str_; }
    std::string str() const { return str_ ? std::string(str_) : ""; }
    explicit operator bool() const { return str_ != nullptr; }
};

// ==================== Helper: create a JS object from key-value pairs ====================
class JsObjectBuilder {
    JSContext* ctx_;
    JSValue obj_;
public:
    JsObjectBuilder(JSContext* ctx)
        : ctx_(ctx), obj_(JS_NewObject(ctx)) {}

    ~JsObjectBuilder() { JS_FreeValue(ctx_, obj_); }

    JsObjectBuilder& set(const char* key, const char* val) {
        JSValue v = JS_NewString(ctx_, val);
        JS_SetPropertyStr(ctx_, obj_, key, v);
        return *this;
    }

    JsObjectBuilder& set(const char* key, const std::string& val) {
        return set(key, val.c_str());
    }

    JsObjectBuilder& set(const char* key, int32_t val) {
        JS_SetPropertyStr(ctx_, obj_, key, JS_NewInt32(ctx_, val));
        return *this;
    }

    JsObjectBuilder& set(const char* key, int64_t val) {
        JS_SetPropertyStr(ctx_, obj_, key, JS_NewInt64(ctx_, val));
        return *this;
    }

    JsObjectBuilder& set(const char* key, double val) {
        JS_SetPropertyStr(ctx_, obj_, key, JS_NewFloat64(ctx_, val));
        return *this;
    }

    JsObjectBuilder& set(const char* key, bool val) {
        JS_SetPropertyStr(ctx_, obj_, key, JS_NewBool(ctx_, val));
        return *this;
    }

    JsObjectBuilder& set(const char* key, JSValue val) {
        JS_SetPropertyStr(ctx_, obj_, key, val);
        return *this;
    }

    JSValue build() {
        JSValue result = obj_;
        obj_ = JS_UNDEFINED;  // prevent destruction
        return result;
    }

    JSValue get() const { return obj_; }
};
