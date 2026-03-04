import { useState, useMemo } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type { PlanDocument } from '../vite-env.d'

interface PlanDocumentWidgetProps {
  planDocument: PlanDocument
}

// Number of lines to show when collapsed
const COLLAPSED_LINES = 5

export default function PlanDocumentWidget({ planDocument }: PlanDocumentWidgetProps) {
  const [isExpanded, setIsExpanded] = useState(false)

  // Split markdown into lines and determine if truncation is needed
  const { displayContent, needsTruncation, totalLines } = useMemo(() => {
    const lines = planDocument.markdown.split('\n')
    const totalLines = lines.length
    const needsTruncation = totalLines > COLLAPSED_LINES

    if (!needsTruncation || isExpanded) {
      return { 
        displayContent: planDocument.markdown, 
        needsTruncation, 
        totalLines 
      }
    }

    // Take first N lines and add ellipsis indicator
    const truncatedLines = lines.slice(0, COLLAPSED_LINES)
    return { 
      displayContent: truncatedLines.join('\n'), 
      needsTruncation, 
      totalLines 
    }
  }, [planDocument.markdown, isExpanded])

  return (
    <div className="plan-document-widget">
      <div className={`plan-document-content ${!isExpanded && needsTruncation ? 'collapsed' : ''}`}>
        <ReactMarkdown
          remarkPlugins={[remarkGfm]}
          components={{
            // Custom code block rendering
            code({ className, children, ...props }) {
              const match = /language-(\w+)/.exec(className || '')
              const isInline = !match && !className

              if (isInline) {
                return <code className="inline-code" {...props}>{children}</code>
              }

              return (
                <div className="code-block">
                  {match && <div className="code-language">{match[1]}</div>}
                  <pre>
                    <code className={className} {...props}>
                      {children}
                    </code>
                  </pre>
                </div>
              )
            },
            // Custom link rendering (open in external browser)
            a({ href, children, ...props }) {
              return (
                <a
                  href={href}
                  target="_blank"
                  rel="noopener noreferrer"
                  {...props}
                >
                  {children}
                </a>
              )
            },
            // Custom table rendering
            table({ children, ...props }) {
              return (
                <div className="table-wrapper">
                  <table {...props}>{children}</table>
                </div>
              )
            },
          }}
        >
          {displayContent}
        </ReactMarkdown>
      </div>

      {needsTruncation && (
        <button
          className="plan-document-expand-button"
          onClick={() => setIsExpanded(!isExpanded)}
        >
          <span className="plan-document-expand-icon">
            {isExpanded ? '▲' : '▼'}
          </span>
          <span>
            {isExpanded 
              ? 'Collapse plan' 
              : `Show full plan (${totalLines} lines)`}
          </span>
        </button>
      )}
    </div>
  )
}
