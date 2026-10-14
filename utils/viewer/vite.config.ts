import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// npm run dev serves the page on :5173 while you work on it. Your server serves
// the built page (npm run build, then viewer/dist/). Add a proxy to your server
// here if you want one during development.
export default defineConfig({
  plugins: [react()],
  server: {
    host: '0.0.0.0',
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
});
