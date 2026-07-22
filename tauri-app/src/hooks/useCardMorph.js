import { useLayoutEffect, useRef } from 'react'

// Usage: const captureCardRect = useCardMorph(selId)
//   - call captureCardRect(id) synchronously, BEFORE the state update that
//     swaps the dashboard card for the sidebar row (or back)
//   - pass whatever value changes as a result of that swap (selId) as the
//     hook's watch argument
//
// On the next layout, whichever element now represents that same target id
// (found via the shared [data-reorder-id] attribute both the dashboard card
// and the sidebar row carry) is animated FROM the position/size it captured
// TO wherever it actually ended up — a real shared-element transition, not
// a generic fade, so the specific card the user clicked visibly becomes the
// specific row it opens into (and the reverse on close).
export function useCardMorph(watchValue) {
  const pending = useRef(null) // { id, rect }

  const capture = (id) => {
    if (id == null) { pending.current = null; return }
    const el = document.querySelector(`[data-reorder-id="${id}"]`)
    pending.current = el ? { id, rect: el.getBoundingClientRect() } : null
  }

  useLayoutEffect(() => {
    const p = pending.current
    pending.current = null
    if (!p) return
    const el = document.querySelector(`[data-reorder-id="${p.id}"]`)
    if (!el || typeof el.animate !== 'function') return
    const to = el.getBoundingClientRect()
    const from = p.rect
    const dx = from.left - to.left
    const dy = from.top - to.top
    const sx = from.width / Math.max(1, to.width)
    const sy = from.height / Math.max(1, to.height)
    // Nothing actually moved/resized (e.g. re-render with no real layout
    // change) — skip rather than play a no-op animation.
    if (Math.abs(dx) < 1 && Math.abs(dy) < 1 && Math.abs(sx - 1) < 0.02 && Math.abs(sy - 1) < 0.02) return
    el.animate(
      [
        { transform: `translate(${dx}px, ${dy}px) scale(${sx}, ${sy})`, transformOrigin: 'top left' },
        { transform: 'translate(0px, 0px) scale(1, 1)', transformOrigin: 'top left' },
      ],
      { duration: 300, easing: 'cubic-bezier(.22, .9, .3, 1)' },
    )
  }, [watchValue])

  return capture
}
