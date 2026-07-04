/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{js,jsx}'],
  // These classes are composed dynamically at runtime (e.g. 's-' + state,
  // 'st-' + lamp), so the string never appears literally in the source for
  // Tailwind's content scanner to find — without safelisting, its tree-shaker
  // silently drops them from the build (this is why the 'discovering' and
  // 'down' states rendered with no colour). Safelist every state variant.
  safelist: [
    's-discovering', 's-settling', 's-ok', 's-okloss', 's-warn', 's-bad', 's-down',
    'st-discovering', 'st-settling', 'st-ok', 'st-okloss', 'st-minor', 'st-loss', 'st-warn', 'st-bad', 'st-down', 'st-silent',
  ],
  darkMode: ['selector', '[data-theme="dark"]'],
  theme: {
    extend: {
      colors: {
        // Semantic tokens backed by CSS variables (styles.css) so the existing
        // light/dark toggle keeps working — Tailwind utilities and the
        // variable-driven theme system share one source of truth.
        bg: 'var(--bg)', panel: 'var(--panel)', panel2: 'var(--panel2)',
        inputbg: 'var(--input-bg)', border: 'var(--border)', border2: 'var(--border2)',
        btnbg: 'var(--btn-bg)', btnhover: 'var(--btn-hover)',
        ink: 'var(--text)', muted: 'var(--muted)', faint: 'var(--faint)', sel: 'var(--sel)',
        accent: 'var(--accent)', accent2: 'var(--accent2)',
        good: 'var(--good)', goodsoft: 'var(--good-soft)',
        warn: 'var(--warn)', warnsoft: 'var(--warn-soft)',
        bad: 'var(--bad)', danger: 'var(--danger)',
        grid: 'var(--grid)', axis: 'var(--axis)',
      },
      fontFamily: {
        // No bundled/remote web fonts — CSP is self-only and this ships
        // offline, so we lean on high-quality system stacks instead of
        // shipping font files or reaching a CDN.
        sans: ['Inter', 'ui-sans-serif', 'system-ui', '-apple-system', 'Segoe UI', 'Roboto', 'sans-serif'],
        mono: ['ui-monospace', 'JetBrains Mono', 'Cascadia Mono', 'SF Mono', 'Consolas', 'monospace'],
      },
      boxShadow: {
        led: '0 0 0 3px color-mix(in srgb, currentColor 22%, transparent), 0 0 6px 1px color-mix(in srgb, currentColor 55%, transparent)',
      },
      backgroundImage: {
        dotgrid: 'radial-gradient(var(--grid) 1px, transparent 1px)',
      },
      backgroundSize: { dotgrid: '18px 18px' },
    },
  },
  plugins: [],
}
