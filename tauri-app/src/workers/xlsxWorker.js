// xlsxWorker.js — builds an .xlsx workbook OFF the main thread, fed
// incrementally so a fleet-wide export never requires one giant blocking
// backend call or one giant main-thread transfer.
//
// HISTORY OF THIS FILE (why it looks like this): the first version moved
// aoa_to_sheet()/write() off the main thread, which was necessary but not
// sufficient — for a full-fleet export, the freeze persisted because (1)
// window.netpulse.exportAllTargetsCsv() was one Tauri call returning one
// potentially enormous string (Tauri JSON-decodes the IPC response on the
// calling thread — the main thread — so a huge single response is itself a
// blocking decode), and (2) postMessage()-ing that whole string to a
// worker performs a structured CLONE, which for a very large string is
// itself a synchronous, main-thread-blocking copy, completely defeating
// the point of offloading the build step.
//
// The fix is architectural, not a bigger buffer: the fleet export is now
// driven per-target from the main thread (reusing the EXISTING, already
// small/bounded exportTargetCsv(id) command — no backend changes needed),
// feeding this worker one target's CSV chunk at a time as a TRANSFERRED
// ArrayBuffer (zero-copy, size-independent) rather than a cloned string.
// Each individual chunk is bounded by one target's data, not the whole
// fleet's — so neither the backend call, the IPC decode, nor the transfer
// scales with fleet size in a way that can single-handedly freeze anything.
// This is also what makes real progress reporting possible (see 'progress'
// messages below), which a single one-shot call never could.
import { utils, write } from 'xlsx'
import { parseCsv } from '../lib/csv.js'

let wb = null
let historyRows = null // accumulates across multiple 'append' messages
let historySheetName = 'Full History'
const decoder = new TextDecoder()

self.onmessage = (e) => {
  // No `e.origin` check here: that guard matters for `window.onmessage`,
  // which can receive a message from an arbitrary cross-origin window,
  // iframe, or opener. This is a dedicated *module* Worker instantiated
  // in App.jsx as `new Worker(new URL('./workers/xlsxWorker.js',
  // import.meta.url), { type: 'module' })` — the browser only ever
  // delivers messages to a dedicated Worker from the same-origin script
  // that created it; there is no channel for another origin to inject a
  // message into it, so an origin comparison here would be theater, not a
  // real boundary. What's still worth validating is the message *shape*
  // (it's a worker message, not a network payload, but still not
  // guaranteed well-formed) so a malformed message degrades gracefully
  // instead of throwing on `msg.type` access below.
  const msg = e.data
  if (!msg || typeof msg !== 'object' || typeof msg.type !== 'string') return
  try {
    if (msg.type === 'start') {
      wb = utils.book_new()
      for (const sheet of msg.sheets) {
        utils.book_append_sheet(wb, utils.aoa_to_sheet(sheet.rows), sheet.name)
      }
      historySheetName = msg.historySheetName || 'Full History'
      historyRows = []
      self.postMessage({ type: 'started' })
      return
    }
    if (msg.type === 'append') {
      // msg.bytes is the transferred ArrayBuffer of one target's raw CSV
      // text (UTF-8 encoded on the main thread before transfer).
      const text = decoder.decode(msg.bytes)
      const rows = parseCsv(text)
      // Every chunk carries its own header row (see manager.hpp's
      // export_target_full_csv) — keep exactly one, from the first chunk,
      // and drop it from every subsequent chunk so they concatenate into
      // one coherent sheet instead of a header repeated per target.
      const dataRows = historyRows.length === 0 ? rows : rows.slice(1)
      for (const r of dataRows) historyRows.push(r)
      self.postMessage({ type: 'progress', rows: historyRows.length })
      return
    }
    if (msg.type === 'finish') {
      utils.book_append_sheet(wb, utils.aoa_to_sheet(historyRows), historySheetName)
      const buf = write(wb, { type: 'array', bookType: 'xlsx' })
      self.postMessage({ type: 'done', ok: true, buffer: buf }, [buf])
      wb = null; historyRows = null
      return
    }
  } catch (err) {
    self.postMessage({ type: 'done', ok: false, error: String(err?.message || err) })
    wb = null; historyRows = null
  }
}
