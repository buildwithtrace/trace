import { useState, useMemo, useRef, useCallback, useEffect, useLayoutEffect } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type { ChatMessage, BridgePayload } from '../vite-env.d'
import PlanDocumentWidget from './PlanDocumentWidget'
import SymbolPreviewWidget from './SymbolPreviewWidget'
import MermaidDiagram from './MermaidDiagram'

interface MessageBubbleProps {
  message: ChatMessage
  postMessage: (payload: BridgePayload) => void
  tabIsStreaming?: boolean
  showActions?: boolean
  activeTabId?: string | null
}

const TRUNCATE_THRESHOLD = 800

const COLLAPSED_MAX_HEIGHT = 100

export default function MessageBubble({ message, postMessage, tabIsStreaming, showActions = true, activeTabId }: MessageBubbleProps) {
  const [isExpanded, setIsExpanded] = useState(false)
  const [isEditing, setIsEditing] = useState(false)
  const [editText, setEditText] = useState(message.content)
  const [menuOpen, setMenuOpen] = useState(false)
  const [copied, setCopied] = useState(false)
  const [userExpanded, setUserExpanded] = useState(false)
  const [isOverflowing, setIsOverflowing] = useState(false)
  const editRef = useRef<HTMLTextAreaElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const contentRef = useRef<HTMLDivElement>(null)
  const isUser = message.role === 'user'
  
  // DISABLED: truncation commented out per request
  const needsTruncation = false // !isUser && message.content.length > TRUNCATE_THRESHOLD && !message.isStreaming
  
  const displayContent = useMemo(() => {
    if (!needsTruncation || isExpanded) {
      return message.content
    }
    const truncated = message.content.slice(0, TRUNCATE_THRESHOLD)
    const lastSpace = truncated.lastIndexOf(' ')
    return (lastSpace > TRUNCATE_THRESHOLD - 50 ? truncated.slice(0, lastSpace) : truncated) + '...'
  }, [message.content, needsTruncation, isExpanded])

  useEffect(() => {
    if (isEditing && editRef.current) {
      editRef.current.focus()
      editRef.current.setSelectionRange(editRef.current.value.length, editRef.current.value.length)
      editRef.current.style.height = 'auto'
      editRef.current.style.height = editRef.current.scrollHeight + 'px'
    }
  }, [isEditing])

  useLayoutEffect(() => {
    if (!isUser || !contentRef.current) return
    const el = contentRef.current
    const prevOverflow = el.style.overflow
    const prevMaxHeight = el.style.maxHeight
    el.style.overflow = 'hidden'
    el.style.maxHeight = 'none'
    const overflows = el.scrollHeight > COLLAPSED_MAX_HEIGHT
    el.style.overflow = prevOverflow
    el.style.maxHeight = prevMaxHeight
    setIsOverflowing(overflows)
  }, [message.content, isUser])

  useEffect(() => {
    if (!userExpanded || !isOverflowing) return
    const handleClickOutside = (e: MouseEvent) => {
      if (contentRef.current && !contentRef.current.contains(e.target as Node)) {
        setUserExpanded(false)
      }
    }
    document.addEventListener('mousedown', handleClickOutside)
    return () => document.removeEventListener('mousedown', handleClickOutside)
  }, [userExpanded, isOverflowing])

  const handleStartEdit = useCallback(() => {
    if (!isUser || message.isStreaming || tabIsStreaming) return
    setEditText(message.content)
    setIsEditing(true)
  }, [isUser, message.content, message.isStreaming, tabIsStreaming])

  const handleCancelEdit = useCallback(() => {
    setIsEditing(false)
    setEditText(message.content)
  }, [message.content])

  const handleSubmitEdit = useCallback(() => {
    const trimmed = editText.trim()
    if (!trimmed || trimmed === message.content) {
      handleCancelEdit()
      return
    }
    postMessage({ type: 'edit_message', messageId: message.id, newContent: trimmed, tabId: activeTabId ?? undefined })
    setIsEditing(false)
  }, [editText, message.content, message.id, postMessage, handleCancelEdit, activeTabId])

  const handleEditKeyDown = useCallback((e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      handleSubmitEdit()
    } else if (e.key === 'Escape') {
      handleCancelEdit()
    }
  }, [handleSubmitEdit, handleCancelEdit])

  const handleUndo = useCallback(() => {
    postMessage({ type: 'undo_to_message', messageId: message.id, tabId: activeTabId ?? undefined })
  }, [message.id, postMessage, activeTabId])

  const handleCopyMessage = useCallback(() => {
    navigator.clipboard.writeText(message.content)
    setCopied(true)
    setTimeout(() => {
      setCopied(false)
      setMenuOpen(false)
    }, 1500)
  }, [message.content])

  useEffect(() => {
    if (!menuOpen) return
    const handleClickOutside = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
        setMenuOpen(false)
      }
    }
    const handleEscape = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setMenuOpen(false)
    }
    document.addEventListener('mousedown', handleClickOutside)
    document.addEventListener('keydown', handleEscape)
    return () => {
      document.removeEventListener('mousedown', handleClickOutside)
      document.removeEventListener('keydown', handleEscape)
    }
  }, [menuOpen])

  return (
    <div className={`message-wrapper ${isUser ? 'user' : 'assistant'} ${message.isStreaming ? 'streaming' : ''}`}>
      <div
        ref={contentRef}
        className={`message-content${isUser && isOverflowing ? (userExpanded ? ' user-expanded' : ' user-collapsed') : ''}`}
        onClick={isUser && isOverflowing && !userExpanded ? (e) => { e.stopPropagation(); setUserExpanded(true) } : undefined}
      >
        {isUser ? (
          <>
            {message.attachments && message.attachments.length > 0 && (
              <div className="attachments-preview sent">
                {message.attachments.map((att, index) => (
                  <div key={index} className="attachment-chip">
                    {att.type.startsWith('image/') && (
                      <div className="attachment-chip-image">
                        <img src={`data:${att.type};base64,${att.data}`} alt={att.name} />
                      </div>
                    )}
                    {!att.type.startsWith('image/') && (
                      <svg viewBox="0 0 24 24" width="14" height="14" fill="currentColor" style={{ flexShrink: 0, opacity: 0.6 }}>
                        <path d="M14,2H6A2,2 0 0,0 4,4V20A2,2 0 0,0 6,22H18A2,2 0 0,0 20,20V8L14,2M18,20H6V4H13V9H18V20Z" />
                      </svg>
                    )}
                    <span className="attachment-chip-name">{att.name}</span>
                  </div>
                ))}
              </div>
            )}
            {isEditing ? (
              <div className="message-edit-container">
                <textarea
                  ref={editRef}
                  className="message-edit-textarea"
                  value={editText}
                  onChange={e => {
                    setEditText(e.target.value)
                    e.target.style.height = 'auto'
                    e.target.style.height = e.target.scrollHeight + 'px'
                  }}
                  onKeyDown={handleEditKeyDown}
                  rows={1}
                />
                <div className="message-edit-actions">
                  <button className="message-edit-cancel" onClick={handleCancelEdit}>Cancel</button>
                  <button className="message-edit-submit" onClick={handleSubmitEdit}>Send</button>
                </div>
              </div>
            ) : (
              <>
                <span className="user-message-text" onClick={handleStartEdit}>{message.content}</span>
                {!message.isStreaming && !tabIsStreaming && (
                  <span className="user-inline-actions">
                    <button className="msg-action-btn" onClick={handleStartEdit} title="Edit message">
                      <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                        <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7" />
                        <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z" />
                      </svg>
                    </button>
                    <button className="msg-action-btn" onClick={handleUndo} title="Undo to this point">
                      <svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                        <polyline points="1 4 1 10 7 10" />
                        <path d="M3.51 15a9 9 0 1 0 2.13-9.36L1 10" />
                      </svg>
                    </button>
                  </span>
                )}
              </>
            )}
          </>
        ) : (
          <>
            {message.content && (
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
            )}
            
            {message.planDocument && (
              <PlanDocumentWidget planDocument={message.planDocument} postMessage={postMessage} activeTabId={activeTabId} />
            )}
            {message.symbolPreview && (
              <SymbolPreviewWidget symbolPreview={message.symbolPreview} />
            )}
          </>
        )}
      </div>
      
      {needsTruncation && !message.planDocument && (
        <button 
          className="expand-button"
          onClick={() => setIsExpanded(!isExpanded)}
        >
          {isExpanded ? 'Show less' : 'Show more'}
        </button>
      )}
      
      {showActions && !isUser && !message.isStreaming && (
        <div className="assistant-actions">
          <div className="assistant-menu-wrapper" ref={menuRef}>
            <button className="assistant-menu-btn" onClick={() => setMenuOpen(prev => !prev)}>
              <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
                <circle cx="5" cy="12" r="2" />
                <circle cx="12" cy="12" r="2" />
                <circle cx="19" cy="12" r="2" />
              </svg>
            </button>
            {menuOpen && (
              <div className="assistant-menu-dropdown">
                <button className="assistant-menu-item" onClick={handleCopyMessage}>
                  {copied ? 'Copied!' : 'Copy Message'}
                </button>
              </div>
            )}
          </div>
        </div>
      )}
    </div>
  )
}
