import { useLayoutEffect, useRef } from 'react'

// Reordering a list via drag/move-buttons re-renders the DOM in the new
// order instantly — there's nothing to animate by default, cards just snap.
// This gives each row a real "slide into place" motion instead:
//
//   1. Before this render, we already have last render's position for every
//      row (captured at the end of the previous run of this same effect).
//   2. After this render, measure each row's NEW position.
//   3. If a row moved, invert it — instantly place it (via a transform)
//      back where it visually was — then animate that transform to zero.
//
// `orderKey` should change only when the ORDER of ids changes (e.g. a
// comma-joined id list), not on every data refresh — otherwise this would
// re-measure (harmlessly, but pointlessly) on every 600ms poll tick.
export function useFlipAnimation(containerRef, orderKey) {
  const prevRects = useRef(new Map())
  // id -> in-flight Animation. A fast drag across 3+ cards can trigger a
  // second swap on the same row before its first 260ms FLIP animation has
  // finished. Without canceling the earlier one, both Animation objects
  // stay active on the same element at once — the Web Animations API
  // doesn't blend two independent transform animations sanely, so the row
  // visibly snapped toward whichever one was winning that frame, which is
  // what "the other cards jump to the opposite side" looked like. This is
  // exactly why only rows NOT currently being dragged need tracking here:
  // the dragged row is excluded below and moves via direct mouse transform.
  const activeAnims = useRef(new Map())
  useLayoutEffect(() => {
    const root = containerRef.current
    if (!root) return
    const nodes = root.querySelectorAll('[data-reorder-id]:not(.reorder-dragging)')
    const next = new Map()
    nodes.forEach((n) => next.set(n.getAttribute('data-reorder-id'), n.getBoundingClientRect()))
    nodes.forEach((n) => {
      const id = n.getAttribute('data-reorder-id')
      const before = prevRects.current.get(id)
      const after = next.get(id)
      if (!before || !after) return
      const dx = before.left - after.left
      const dy = before.top - after.top
      if (Math.abs(dx) < 1 && Math.abs(dy) < 1) return
      if (typeof n.animate !== 'function') return // very old webview fallback: just skip the animation
      const prevAnim = activeAnims.current.get(id)
      if (prevAnim) prevAnim.cancel()
      const anim = n.animate(
        [{ transform: `translate(${dx}px, ${dy}px)` }, { transform: 'translate(0, 0)' }],
        { duration: 260, easing: 'cubic-bezier(.65,0,.35,1)' },
      )
      activeAnims.current.set(id, anim)
      anim.onfinish = anim.oncancel = () => {
        if (activeAnims.current.get(id) === anim) activeAnims.current.delete(id)
      }
    })
    prevRects.current = next
  }, [orderKey])
}
