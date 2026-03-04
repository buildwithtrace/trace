import { useState, useMemo } from 'react'
import ReactMarkdown from 'react-markdown'
import remarkGfm from 'remark-gfm'
import type { ChatMessage } from '../vite-env.d'
import PlanDocumentWidget from './PlanDocumentWidget'

interface MessageBubbleProps {
  message: ChatMessage
}

// Character threshold for truncation (AI messages only)
const TRUNCATE_THRESHOLD = 800

export default function MessageBubble({ message }: MessageBubbleProps) {
  const [isExpanded, setIsExpanded] = useState(false)
  const isUser = message.role === 'user'
  
  // Determine if message needs truncation
  const needsTruncation = !isUser && message.content.length > TRUNCATE_THRESHOLD && !message.isStreaming
  
  // Get display content (truncated or full)
  const displayContent = useMemo(() => {
    if (!needsTruncation || isExpanded) {
      return message.content
    }
    // Truncate at word boundary
    const truncated = message.content.slice(0, TRUNCATE_THRESHOLD)
    const lastSpace = truncated.lastIndexOf(' ')
    return (lastSpace > TRUNCATE_THRESHOLD - 50 ? truncated.slice(0, lastSpace) : truncated) + '...'
  }, [message.content, needsTruncation, isExpanded])

  return (
    <div className={`message-wrapper ${isUser ? 'user' : 'assistant'} ${message.isStreaming ? 'streaming' : ''}`}>
      <div className="message-content">
        {isUser ? (
          // User messages: plain text in bubble + attachments
          <>
            {/* Render attachments first */}
            {message.attachments && message.attachments.length > 0 && (
              <div className="message-attachments">
                {message.attachments.map((att, index) => (
                  <div key={index} className="message-attachment">
                    {att.type.startsWith('image/') ? (
                      <img 
                        src={`data:${att.type};base64,${att.data}`} 
                        alt={att.name}
                        className="message-attachment-image"
                      />
                    ) : (
                      <div className="message-attachment-file">
                        <svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor">
                          <path d="M14,2H6A2,2 0 0,0 4,4V20A2,2 0 0,0 6,22H18A2,2 0 0,0 20,20V8L14,2M18,20H6V4H13V9H18V20Z" />
                        </svg>
                        <span>{att.name}</span>
                      </div>
                    )}
                  </div>
                ))}
              </div>
            )}
            {/* Render text content */}
            <span>{message.content}</span>
          </>
        ) : (
          // AI messages: markdown rendering + optional plan document
          <>
            {/* Render text content if present */}
            {message.content && (
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
            )}
            
            {/* Render plan document widget if present */}
            {message.planDocument && (
              <PlanDocumentWidget planDocument={message.planDocument} />
            )}
          </>
        )}
      </div>
      
      {/* Expand/collapse button for long messages (not for plan documents) */}
      {needsTruncation && !message.planDocument && (
        <button 
          className="expand-button"
          onClick={() => setIsExpanded(!isExpanded)}
        >
          {isExpanded ? 'Show less' : 'Show more'}
        </button>
      )}
    </div>
  )
}
