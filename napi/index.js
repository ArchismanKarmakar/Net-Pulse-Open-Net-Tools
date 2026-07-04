// Loads the CMake.js-built addon. cmake-js emits to build/Release on
// multi-config generators and build/ on single-config — try both.
let addon
try { addon = require('./build/Release/netpulse.node') }
catch (_) { addon = require('./build/netpulse.node') }

module.exports = {
  addTarget: (opts) => addon.addTarget(opts),
  updateTarget: (id, opts) => addon.updateTarget(id, opts),
  pauseTarget: (id, on) => addon.pauseTarget(id, !!on),
  stopTarget: (id) => addon.stopTarget(id),
  removeTarget: (id) => addon.removeTarget(id),
  listInterfaces: () => addon.listInterfaces(),
  getState: (focus) => JSON.parse(addon.getState(typeof focus === 'number' ? focus : undefined)),
  getStateJSON: (focus) => addon.getState(typeof focus === 'number' ? focus : undefined),
}
