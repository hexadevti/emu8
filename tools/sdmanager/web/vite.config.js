import { defineConfig } from 'vite';

export default defineConfig({
  // Relative asset URLs, so the built dist/ works from any folder or sub-path (GitHub Pages, a NAS…).
  base: './',
  // Web Serial only works in a secure context: http://localhost counts, a LAN IP does not.
  // So the dev and preview servers stay on localhost.
  // SDM_NO_OPEN is set by the VS Code launch (.vscode/launch.json), which opens its own debug browser.
  server: { port: 5180, open: !process.env.SDM_NO_OPEN, strictPort: true },
  preview: { port: 5180, open: true },
  build: { outDir: 'dist', emptyOutDir: true, target: 'es2022' },
});
