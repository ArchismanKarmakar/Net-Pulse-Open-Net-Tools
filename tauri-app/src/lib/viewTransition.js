export function withViewTransition(fn) {
  if (typeof document !== 'undefined' && typeof document.startViewTransition === 'function') {
    document.startViewTransition(fn)
  } else {
    fn()
  }
}
