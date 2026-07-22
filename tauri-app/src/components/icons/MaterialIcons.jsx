import React from 'react'

// Material Symbols (Google, Apache-2.0 license — see THIRD_PARTY_NOTICES.md),
// outlined style, downloaded from google/material-design-icons and inlined
// here as plain SVG rather than pulled in via an icon font or a large icon
// package. Two reasons: (1) this app deliberately ships no web fonts (see
// README.md's Styling section and THIRD_PARTY_NOTICES.md's Fonts section —
// an icon font is still a font, with the same licensing/CSP considerations);
// (2) only 5 specific icons are actually used, so bundling a handful of
// ~600-byte SVGs is far lighter than a whole icon-font/npm-package
// dependency for 5 glyphs.
//
// `fill="currentColor"` on every path (not hardcoded in the original
// downloaded SVGs, which default to black) is what makes these correctly
// pick up the button's text color — including on hover and in both the
// light and dark themes, with zero extra CSS needed.
const iconProps = {
  viewBox: '0 -960 960 960',
  width: '17',
  height: '17',
  fill: 'currentColor',
  'aria-hidden': 'true',
  focusable: 'false',
}

// "image" — Graph PNG export (a picture of the latency chart).
export function IconImage(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M200-120q-33 0-56.5-23.5T120-200v-560q0-33 23.5-56.5T200-840h560q33 0 56.5 23.5T840-760v560q0 33-23.5 56.5T760-120H200Zm0-80h560v-560H200v560Zm40-80h480L570-480 450-320l-90-120-120 160Zm-40 80v-560 560Z" />
    </svg>
  )
}

// "csv" — Hops CSV export (current per-hop summary).
export function IconCsv(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M230-360h120v-60H250v-120h100v-60H230q-17 0-28.5 11.5T190-560v160q0 17 11.5 28.5T230-360Zm156 0h120q17 0 28.5-11.5T546-400v-60q0-17-11.5-31.5T506-506h-60v-34h100v-60H426q-17 0-28.5 11.5T386-560v60q0 17 11.5 30.5T426-456h60v36H386v60Zm264 0h60l70-240h-60l-40 138-40-138h-60l70 240ZM160-160q-33 0-56.5-23.5T80-240v-480q0-33 23.5-56.5T160-800h640q33 0 56.5 23.5T880-720v480q0 33-23.5 56.5T800-160H160Zm0-80h640v-480H160v480Zm0 0v-480 480Z" />
    </svg>
  )
}

// "route" — Traceroute PNG export (a picture of the hop table itself).
export function IconRoute(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M360-120q-66 0-113-47t-47-113v-327q-35-13-57.5-43.5T120-720q0-50 35-85t85-35q50 0 85 35t35 85q0 39-22.5 69.5T280-607v327q0 33 23.5 56.5T360-200q33 0 56.5-23.5T440-280v-400q0-66 47-113t113-47q66 0 113 47t47 113v327q35 13 57.5 43.5T840-240q0 50-35 85t-85 35q-50 0-85-35t-35-85q0-39 22.5-70t57.5-43v-327q0-33-23.5-56.5T600-760q-33 0-56.5 23.5T520-680v400q0 66-47 113t-113 47ZM240-680q17 0 28.5-11.5T280-720q0-17-11.5-28.5T240-760q-17 0-28.5 11.5T200-720q0 17 11.5 28.5T240-680Zm480 480q17 0 28.5-11.5T760-240q0-17-11.5-28.5T720-280q-17 0-28.5 11.5T680-240q0 17 11.5 28.5T720-200ZM240-720Zm480 480Z" />
    </svg>
  )
}

// "backup_table" — Full CSV export (every recorded sample, not just
// current). Deliberately not "history": two overlapping tables reads more
// directly as "the complete/backup copy of the data" and is visually more
// distinct from IconCsv (a single file) than a clock-based icon was.
export function IconBackupTable(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M320-320h200v-200H320v200Zm0-280h480v-200H320v200Zm280 280h200v-200H600v200Zm-280 80q-33 0-56.5-23.5T240-320v-480q0-33 23.5-56.5T320-880h480q33 0 56.5 23.5T880-800v480q0 33-23.5 56.5T800-240H320ZM160-80q-33 0-56.5-23.5T80-160v-560h80v560h560v80H160Z" />
    </svg>
  )
}

// "table_chart" — Excel export.
export function IconTableChart(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M760-120H200q-33 0-56.5-23.5T120-200v-560q0-33 23.5-56.5T200-840h560q33 0 56.5 23.5T840-760v560q0 33-23.5 56.5T760-120ZM200-640h560v-120H200v120Zm100 80H200v360h100v-360Zm360 0v360h100v-360H660Zm-80 0H380v360h200v-360Z" />
    </svg>
  )
}

// "save" — Save target list to .npulse.
export function IconSave(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M840-680v480q0 33-23.5 56.5T760-120H200q-33 0-56.5-23.5T120-200v-560q0-33 23.5-56.5T200-840h480l160 160Zm-80 34L646-760H200v560h560v-446ZM480-240q50 0 85-35t35-85q0-50-35-85t-85-35q-50 0-85 35t-35 85q0 50 35 85t85 35ZM240-560h360v-160H240v160Zm-40-86v446-560 114Z" />
    </svg>
  )
}

// "upload" — Load target list from .npulse.
export function IconUpload(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M440-320v-326L336-542l-56-58 200-200 200 200-56 58-104-104v326h-80ZM240-160q-33 0-56.5-23.5T160-240v-120h80v120h480v-120h80v120q0 33-23.5 56.5T720-160H240Z" />
    </svg>
  )
}

// "data_object" — Export JSON ({} braces — the standard JSON glyph).
export function IconDataObject(props) {
  return (
    <svg {...iconProps} {...props}>
      <path d="M560-160v-80h120q17 0 28.5-11.5T720-280v-80q0-38 22-69t58-44v-14q-36-13-58-44t-22-69v-80q0-17-11.5-28.5T680-720H560v-80h120q50 0 85 35t35 85v80q0 17 11.5 28.5T840-560h40v160h-40q-17 0-28.5 11.5T800-360v80q0 50-35 85t-85 35H560Zm-280 0q-50 0-85-35t-35-85v-80q0-17-11.5-28.5T120-400H80v-160h40q17 0 28.5-11.5T160-600v-80q0-50 35-85t85-35h120v80H280q-17 0-28.5 11.5T240-680v80q0 38-22 69t-58 44v14q36 13 58 44t22 69v80q0 17 11.5 28.5T280-240h120v80H280Z" />
    </svg>
  )
}
