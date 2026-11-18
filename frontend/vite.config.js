import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// During `npm run dev` the React app runs on :5173 and forwards API/WebSocket calls
// to the FastAPI backend on :8000. In the demo you use `npm run build` instead and
// FastAPI serves the built files itself, so everything is on one port.
export default defineConfig({
  plugins: [react()],
  build: { chunkSizeWarningLimit: 1000 }, // recharts is big; fine for a local demo
  server: {
    proxy: {
      '/api': 'http://127.0.0.1:8000',
      '/ws': { target: 'ws://127.0.0.1:8000', ws: true },
    },
  },
})
