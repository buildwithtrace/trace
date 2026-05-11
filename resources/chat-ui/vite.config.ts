import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

// Custom plugin to remove type="module" from script tags for webview compatibility
// Must run AFTER viteSingleFile to modify the final HTML
const removeModuleType = () => ({
  name: 'remove-module-type',
  enforce: 'post' as const,
  generateBundle(_options: unknown, bundle: Record<string, { type: string; source?: string }>) {
    for (const file of Object.values(bundle)) {
      if (file.type === 'asset' && typeof file.source === 'string' && file.source.includes('<!DOCTYPE html>')) {
        // Remove type="module" and crossorigin attributes from script tags
        file.source = file.source
          .replace(/<script type="module" crossorigin>/g, '<script>')
          .replace(/<script type="module">/g, '<script>')
      }
    }
  }
})

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [
    react(),
    viteSingleFile({
      removeViteModuleLoader: true,
    }),
    removeModuleType(),
  ],
  build: {
    // Output a single HTML file with all assets inlined
    cssCodeSplit: false,
    assetsInlineLimit: 100000000, // Inline everything
    // Use IIFE format instead of ES modules for webview compatibility
    target: 'es2015',
    modulePreload: false,
    rollupOptions: {
      output: {
        manualChunks: undefined,
        format: 'iife',
        inlineDynamicImports: true,
      },
    },
  },
})
