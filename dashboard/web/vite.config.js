import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The browser only ever talks to this origin; /api (REST + SSE) is proxied to the local server.
export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
    proxy: {
      '/api': {
        target: process.env.API_URL ?? 'http://127.0.0.1:8787', // API_URL: point the panel at another backend
        changeOrigin: true,
        // When the server dies mid-stream the proxy would leave the browser's SSE response open forever (no error, no reconnect,
        // a LIVE panel that hears nothing). Cut it, so EventSource notices and reconnects.
        configure: proxy => proxy.on('proxyRes', (proxyRes, _req, res) => proxyRes.on('close', () => res.writableEnded || res.destroy())),
      },
    },
  },
});
