// Loads the CMake.js-built addon. cmake-js emits to build/Release on
// multi-config generators and build/ on single-config — try both.
let addon
try { addon = require('./build/Release/netpulse.node') }
catch (_) { addon = require('./build/netpulse.node') }

module.exports = {
  // Build tag of the compiled native engine (see NETPULSE_ENGINE_BUILD in
  // napi.cpp). Forwarded so the app / logs can verify the addon was recompiled.
  engineBuild: addon.engineBuild,
  getEngineBuild: () => (addon.getEngineBuild ? addon.getEngineBuild() : addon.engineBuild),
  addTarget: (opts) => addon.addTarget(opts),
  updateTarget: (id, opts) => addon.updateTarget(id, opts),
  pauseTarget: (id, on) => addon.pauseTarget(id, !!on),
  stopTarget: (id) => addon.stopTarget(id),
  removeTarget: (id) => addon.removeTarget(id),
  listInterfaces: () => addon.listInterfaces(),
  // getState(focusSeconds?) -> Promise<parsed object>, identical shape to the
  // old /api/state. The native call runs on N-API's worker thread pool (see
  // napi.cpp GetStateWorker), so it never blocks Electron's main thread.
  getState: (focus) => addon.getState(typeof focus === 'number' ? focus : undefined).then((s) => JSON.parse(s)),
  getStateJSON: (focus) => addon.getState(typeof focus === 'number' ? focus : undefined),
}
