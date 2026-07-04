// NetPulse Node-API addon — exposes the verified C++ engine (the same
// netpulse::Manager used by the HTTP server) directly to Electron's main
// process. No localhost port, no HTTP: the renderer calls these functions in
// process. Probing runs on the engine's own background threads; getState()
// returns the latest JSON snapshot (identical shape to the old /api/state).
//
// Build:  npm install   (runs node-gyp; needs a C++17 toolchain)
//         for Electron:  npx electron-rebuild   (matches Electron's ABI)

#include <napi.h>

#include "netpulse/manager.hpp"

using namespace netpulse;

// One engine instance for the process. Its destructor joins all target threads.
static Manager& mgr() {
    static Manager m;
    return m;
}

// Build Settings from a JS options object, starting from `base` so partial
// updates (live edits) only touch the fields that are present. All values are
// clamped to safe ranges so hostile/garbage input can't drive the engine into
// huge allocations or unbounded loops.
static Settings settings_from_obj(const Napi::Object& o, Settings base = Settings{}) {
    auto num = [&](const char* k, double def) {
        return o.Has(k) && !o.Get(k).IsUndefined() ? o.Get(k).ToNumber().DoubleValue() : def;
    };
    auto clamp = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };
    base.probe_interval = clamp(num("probe", base.probe_interval), 0.01, 3600.0);
    base.trace_interval = clamp(num("trace", base.trace_interval), 1.0, 86400.0);
    base.timeout = clamp(num("timeout", base.timeout), 0.0, 3600.0);
    base.payload_size = static_cast<size_t>(clamp(num("payload", static_cast<double>(base.payload_size)), 0.0, 1472.0));
    base.max_hops = static_cast<uint8_t>(clamp(num("maxhops", static_cast<double>(base.max_hops)), 1.0, 64.0));
    if (o.Has("raw")) base.privileged = o.Get("raw").ToBoolean().Value();
    if (o.Has("src")) {
        std::string s = o.Get("src").ToString().Utf8Value();
        if (s.size() <= 64) base.source_addr = s; // ignore absurd input
    }
    if (o.Has("family")) {
        std::string f = o.Get("family").ToString().Utf8Value();
        base.family = f == "v4" ? FamilyPref::V4 : f == "v6" ? FamilyPref::V6 : FamilyPref::Auto;
    }
    base.focus_secs = std::nullopt; // focus is applied per getState() call
    return base;
}

static Napi::Value AddTarget(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    try {
        if (info.Length() < 1 || !info[0].IsObject())
            throw Napi::TypeError::New(env, "addTarget(options) expects an object");
        Napi::Object o = info[0].As<Napi::Object>();
        std::string target = o.Get("target").ToString().Utf8Value();
        if (target.empty() || target.size() > 255)
            throw Napi::TypeError::New(env, "addTarget: invalid target");
        uint64_t id = mgr().add(target, settings_from_obj(o));
        return Napi::Number::New(env, static_cast<double>(id));
    } catch (const Napi::Error&) {
        throw;
    } catch (const std::exception& e) {
        throw Napi::Error::New(env, std::string("addTarget failed: ") + e.what());
    }
}

static Napi::Value UpdateTarget(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    try {
        if (info.Length() < 2 || !info[1].IsObject())
            throw Napi::TypeError::New(env, "updateTarget(id, options) expects (number, object)");
        uint64_t id = static_cast<uint64_t>(info[0].ToNumber().Int64Value());
        Napi::Object o = info[1].As<Napi::Object>();
        Settings cur = mgr().settings_of(id).value_or(Settings{});
        bool ok = mgr().update(id, settings_from_obj(o, cur));
        return Napi::Boolean::New(env, ok);
    } catch (const Napi::Error&) {
        throw;
    } catch (const std::exception& e) {
        throw Napi::Error::New(env, std::string("updateTarget failed: ") + e.what());
    }
}

static Napi::Value PauseTarget(const Napi::CallbackInfo& info) {
    mgr().pause(static_cast<uint64_t>(info[0].ToNumber().Int64Value()), info[1].ToBoolean().Value());
    return info.Env().Undefined();
}
static Napi::Value StopTarget(const Napi::CallbackInfo& info) {
    mgr().stop(static_cast<uint64_t>(info[0].ToNumber().Int64Value()));
    return info.Env().Undefined();
}
static Napi::Value RemoveTarget(const Napi::CallbackInfo& info) {
    mgr().remove(static_cast<uint64_t>(info[0].ToNumber().Int64Value()));
    return info.Env().Undefined();
}

static Napi::Value ListInterfaces(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    auto ifs = list_interfaces();
    Napi::Array arr = Napi::Array::New(env, ifs.size());
    for (size_t i = 0; i < ifs.size(); ++i) {
        Napi::Object o = Napi::Object::New(env);
        o.Set("name", ifs[i].name);
        o.Set("address", ifs[i].address);
        o.Set("v6", ifs[i].v6);
        arr.Set(i, o);
    }
    return arr;
}

// Building the JSON snapshot (stats + downsampled series for every hop of
// every target) does real work — this runs it on N-API's worker thread pool
// via AsyncWorker instead of on the calling thread. That matters specifically
// because the caller here is Electron's MAIN process: a synchronous native
// call there blocks not just this one IPC response but the whole process's
// event loop (window messages, other IPC, compositing scheduling) for as long
// as the call takes. Returning a Promise keeps the main thread free the whole
// time; ipcMain.handle() already awaits promises transparently.
class GetStateWorker : public Napi::AsyncWorker {
public:
    GetStateWorker(Napi::Env env, std::optional<double> focus)
        : Napi::AsyncWorker(env), focus_(focus), deferred_(Napi::Promise::Deferred::New(env)) {}
    void Execute() override { result_ = mgr().state_json(focus_); }
    void OnOK() override {
        Napi::HandleScope scope(Env());
        deferred_.Resolve(Napi::String::New(Env(), result_));
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::optional<double> focus_;
    std::string result_;
    Napi::Promise::Deferred deferred_;
};

// Returns a Promise<string> of the same JSON shape the HTTP server used to
// serve at /api/state, so the renderer's parsing code is unchanged — only the
// transport (now async, in-process) is different.
static Napi::Value GetState(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::optional<double> focus;
    if (info.Length() > 0 && info[0].IsNumber()) focus = info[0].ToNumber().DoubleValue();
    auto* worker = new GetStateWorker(env, focus);
    worker->Queue();
    return worker->GetPromise();
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("addTarget", Napi::Function::New(env, AddTarget));
    exports.Set("updateTarget", Napi::Function::New(env, UpdateTarget));
    exports.Set("pauseTarget", Napi::Function::New(env, PauseTarget));
    exports.Set("stopTarget", Napi::Function::New(env, StopTarget));
    exports.Set("removeTarget", Napi::Function::New(env, RemoveTarget));
    exports.Set("listInterfaces", Napi::Function::New(env, ListInterfaces));
    exports.Set("getState", Napi::Function::New(env, GetState));
    return exports;
}

NODE_API_MODULE(netpulse, Init)
