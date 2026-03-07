import { useState, useMemo, useRef, useEffect } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type { PlanDocument, BridgePayload } from '../vite-env.d'
import MermaidDiagram from './MermaidDiagram'

interface PlanDocumentWidgetProps {
  planDocument: PlanDocument
  postMessage: (payload: BridgePayload) => void
  activeTabId?: string | null
}

const COLLAPSED_LINES = 5

export default function PlanDocumentWidget({ planDocument, postMessage, activeTabId }: PlanDocumentWidgetProps) {
  const [isExpanded, setIsExpanded] = useState(false)
  const [isEditing, setIsEditing] = useState(false)
  const [editText, setEditText] = useState('')
  const [saveStatus, setSaveStatus] = useState<'idle' | 'saved'>('idle')
  const textareaRef = useRef<HTMLTextAreaElement>(null)

  const { displayContent, needsTruncation, totalLines } = useMemo(() => {
    const md = planDocument.markdown || ''
    const lines = md.split('\n')
    const totalLines = lines.length
    const needsTruncation = totalLines > COLLAPSED_LINES

    if (!needsTruncation || isExpanded) {
      return { 
        displayContent: md, 
        needsTruncation, 
        totalLines 
      }
    }

    const truncatedLines = lines.slice(0, COLLAPSED_LINES)
    return { 
      displayContent: truncatedLines.join('\n'), 
      needsTruncation, 
      totalLines 
    }
  }, [planDocument.markdown, isExpanded])

  useEffect(() => {
    if (isEditing && textareaRef.current) {
      textareaRef.current.focus()
      textareaRef.current.setSelectionRange(
        textareaRef.current.value.length,
        textareaRef.current.value.length
      )
    }
  }, [isEditing])

  const handleEditClick = () => {
    setEditText(planDocument.markdown)
    setIsEditing(true)
    setIsExpanded(true)
  }

  const handleSaveToWorkspace = () => {
    postMessage({ type: 'plan_save_to_workspace', markdown: planDocument.markdown })
    setSaveStatus('saved')
  }

  useEffect(() => {
    if (saveStatus !== 'saved') return
    const id = setTimeout(() => setSaveStatus('idle'), 2500)
    return () => clearTimeout(id)
  }, [saveStatus])

  const handleCancel = () => {
    setIsEditing(false)
    setEditText('')
  }

  const handleSaveAndExecute = () => {
    if (!editText.trim()) return
    postMessage({ type: 'plan_edit_save', markdown: editText, tabId: activeTabId ?? undefined })
    setIsEditing(false)
    setEditText('')
  }

  if (isEditing) {
    return (
      <div className="plan-document-widget">
        <div className="plan-edit-header">
          <span className="plan-edit-title">Edit Plan</span>
        </div>
        <textarea
          ref={textareaRef}
          className="plan-edit-textarea"
          value={editText}
          onChange={(e) => setEditText(e.target.value)}
          spellCheck={false}
        />
        <div className="plan-edit-actions">
          <button className="plan-edit-cancel" onClick={handleCancel}>
            Cancel
          </button>
          <button
            className="plan-edit-save"
            onClick={handleSaveAndExecute}
            disabled={!editText.trim()}
          >
            Save &amp; Execute
          </button>
        </div>
      </div>
    )
  }

  return (
    <div className="plan-document-widget">
      <div className={`plan-document-content ${!isExpanded && needsTruncation ? 'collapsed' : ''}`}>
        <ReactMarkdown
          remarkPlugins={[remarkGfm]}
          components={{
            code({ className, children, ...props }) {
              const match = /language-(\w+)/.exec(className || '')
              const isInline = !match && !className

              if (isInline) {
                return <code className="inline-code" {...props}>{children}</code>
              }

              if (match && match[1] === 'mermaid') {
                const chart = String(children).replace(/\n$/, '')
                return <MermaidDiagram chart={chart} />
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

      <div className="plan-document-footer">
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
        {planDocument.renderMode === 'full' && (
          <button className="plan-edit-button" onClick={handleEditClick}>
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7" />
              <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z" />
            </svg>
            <span>Edit</span>
          </button>
        )}
        <button
          className={`plan-save-button ${saveStatus === 'saved' ? 'plan-save-button-saved' : ''}`}
          onClick={handleSaveToWorkspace}
          disabled={saveStatus === 'saved'}
        >
          {saveStatus === 'saved' ? (
            <>
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <polyline points="20 6 9 17 4 12" />
              </svg>
              <span>Saved</span>
            </>
          ) : (
            <>
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z" />
                <polyline points="17 21 17 13 7 13 7 21" />
                <polyline points="7 3 7 8 15 8" />
              </svg>
              <span>Save to Workspace</span>
            </>
          )}
        </button>
      </div>
    </div>
  )
}
