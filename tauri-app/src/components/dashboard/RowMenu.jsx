import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'

// items: [{ label, onClick, danger? }]
export default function RowMenu({ items }) {
  const [open, setOpen] = useState(false)
  const [pos, setPos] = useState(null)
  const btnRef = useRef(null)
  const dropRef = useRef(null)

  useEffect(() => {
    if (!open) return
    const away = (e) => {
      if (dropRef.current && dropRef.current.contains(e.target)) return
      if (btnRef.current && btnRef.current.contains(e.target)) return
      setOpen(false)
    }
    // A resize/scroll while open would leave the portal pinned to a stale
    // screen position (it's fixed, not anchored in the DOM flow) — just
    // close it rather than tracking every possible scroll container.
    const closeOnMove = () => setOpen(false)
    window.addEventListener('mousedown', away)
    window.addEventListener('resize', closeOnMove)
    window.addEventListener('scroll', closeOnMove, true)
    return () => {
      window.removeEventListener('mousedown', away)
      window.removeEventListener('resize', closeOnMove)
      window.removeEventListener('scroll', closeOnMove, true)
    }
  }, [open])

  const toggle = () => {
    if (!open && btnRef.current) {
      const r = btnRef.current.getBoundingClientRect()
      setPos({ top: r.bottom + 4, left: r.right }) // right-aligned to the button, like before
    }
    setOpen((o) => !o)
  }

  return (
    <>
      <div className="row-menu" onClick={(e) => e.stopPropagation()}>
        <button ref={btnRef} className="row-menu-btn" title="Actions" aria-label="Actions" onClick={toggle}>⋮</button>
      </div>
      {open && pos && createPortal(
        <div
          ref={dropRef}
          className="row-menu-drop"
          style={{ position: 'fixed', top: pos.top, left: pos.left, transform: 'translateX(-100%)' }}
          onClick={(e) => e.stopPropagation()}
        >
          {items.map((it, i) => (
            <button key={i} className={'row-menu-item' + (it.danger ? ' danger' : '')}
              onClick={() => { setOpen(false); it.onClick() }}>{it.label}</button>
          ))}
        </div>,
        document.body,
      )}
    </>
  )
}
