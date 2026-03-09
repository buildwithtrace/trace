import React from 'react'
import ReactDOM from 'react-dom/client'
import App from './App.tsx'
import './styles/chat.css'

function initApp() {
  const rootElement = document.getElementById('root')

  if (rootElement) {
    ReactDOM.createRoot(rootElement).render(
      <React.StrictMode>
        <App />
      </React.StrictMode>,
    )
  } else {
    console.error('[Trace] No root element found!')
  }
}

// Wait for DOM to be ready before initializing React
// The script is in <head> but the root element is in <body>
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initApp)
} else {
  initApp()
}
