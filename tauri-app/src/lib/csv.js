// Minimal RFC 4180 CSV parser (single pass, handles quoted fields with
// embedded commas/newlines and doubled internal quotes) — the backend's
// full-history export properly CSV-escapes now (see csv_esc in
// manager.hpp), so this is the correct counterpart to read it back into
// rows for the XLSX sheets, rather than a naive text.split(',') that would
// silently break on a target name or hostname containing a comma.
// `#`-prefixed comment lines (the export's own leading metadata) are
// dropped; everything else becomes one row per line, one cell per field.
//
// A standalone module (not a closure inside App.jsx) specifically so the
// xlsx export Web Worker (workers/xlsxWorker.js) can import it too — a
// worker has no access to anything defined inside the React component.
export function parseCsv(text) {
  const rows = []
  let row = [], field = '', inQuotes = false, i = 0
  const n = text.length
  while (i < n) {
    const c = text[i]
    if (inQuotes) {
      if (c === '"') {
        if (text[i + 1] === '"') { field += '"'; i += 2; continue }
        inQuotes = false; i++; continue
      }
      field += c; i++; continue
    }
    if (c === '"') { inQuotes = true; i++; continue }
    if (c === ',') { row.push(field); field = ''; i++; continue }
    if (c === '\r') { i++; continue }
    if (c === '\n') { row.push(field); field = ''; rows.push(row); row = []; i++; continue }
    field += c; i++
  }
  if (field !== '' || row.length) { row.push(field); rows.push(row) }
  return rows.filter((r) => !(r.length === 1 && r[0].startsWith('#')))
}
