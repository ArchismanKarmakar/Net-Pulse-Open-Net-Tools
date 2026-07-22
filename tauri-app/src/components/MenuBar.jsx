import { useEffect, useRef, useState } from 'react'

// Each item is { label, onClick, checked?, sep?, disabled?, hint? }.
export default function MenuBar({ menus }) {
  const [open, setOpen] = useState(null)
  const ref = useRef(null)
  useEffect(() => {
    if (open == null) return
    const away = (e) => { if (ref.current && !ref.current.contains(e.target)) setOpen(null) }
    const esc = (e) => { if (e.key === 'Escape') setOpen(null) }
    window.addEventListener('mousedown', away); window.addEventListener('keydown', esc)
    return () => { window.removeEventListener('mousedown', away); window.removeEventListener('keydown', esc) }
  }, [open])
  return (
    <nav className="menubar" ref={ref}>
      {menus.map((m, i) => (
        <div key={i} className={'menu' + (open === i ? ' open' : '')}>
          <button className="menu-top" onClick={() => setOpen(open === i ? null : i)}
            onMouseEnter={() => open != null && setOpen(i)}>{m.label}</button>
          {open === i && (
            <div className="menu-drop">
              {m.items.filter(Boolean).map((it, j) => it.sep
                ? <div key={j} className="menu-sep" />
                : (
                  <button key={j} className="menu-item" disabled={it.disabled}
                    onClick={() => { setOpen(null); it.onClick && it.onClick() }} title={it.hint || ''}>
                    <span className="menu-check">{it.checked ? '✓' : ''}</span>
                    <span className="menu-label">{it.label}</span>
                  </button>
                ))}
            </div>
          )}
        </div>
      ))}
    </nav>
  )
}
