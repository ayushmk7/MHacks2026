import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The browser only ever talks to this origin; /api (REST + SSE) is proxied to the local server.
export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
    proxy: { '/api': process.env.API_URL ?? 'http://127.0.0.1:8787' }, // API_URL: point the panel at another backend
  },
});
