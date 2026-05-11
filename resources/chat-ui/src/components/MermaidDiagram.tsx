import { useEffect, useRef, useState } from 'react'
import mermaid from 'mermaid'

let mermaidInitialized = false
let diagramCounter = 0

export default function MermaidDiagram({ chart }: { chart: string }) {
  const containerRef = useRef<HTMLDivElement>(null)
  const [svg, setSvg] = useState<string>('')
  const [error, setError] = useState<string>('')
  const idRef = useRef(`mermaid-${Date.now()}-${diagramCounter++}`)

  useEffect(() => {
    if (!chart.trim()) return

    if (!mermaidInitialized) {
      const isDark = document.documentElement.classList.contains('dark')
      mermaid.initialize({
        startOnLoad: false,
        theme: isDark ? 'dark' : 'default',
        securityLevel: 'loose',
        fontFamily: 'inherit',
      })
      mermaidInitialized = true
    }

    let cancelled = false

    mermaid.render(idRef.current, chart.trim()).then(({ svg: renderedSvg }) => {
      if (!cancelled) {
        setSvg(renderedSvg)
        setError('')
      }
    }).catch((err) => {
      if (!cancelled) {
        setError(String(err?.message || err))
        setSvg('')
      }
    })

    return () => { cancelled = true }
  }, [chart])

  if (error) {
    return (
      <div className="mermaid-error">
        <div className="code-block">
          <div className="code-language">mermaid (render error)</div>
          <pre><code>{chart}</code></pre>
        </div>
      </div>
    )
  }

  if (!svg) {
    return (
      <div className="mermaid-loading">
        <span>Rendering diagram...</span>
      </div>
    )
  }

  return (
    <div
      ref={containerRef}
      className="mermaid-diagram"
      dangerouslySetInnerHTML={{ __html: svg }}
    />
  )
}
