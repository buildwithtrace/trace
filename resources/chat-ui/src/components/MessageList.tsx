import { useEffect, useRef } from 'react'
import MessageBubble from './MessageBubble'
import TypingIndicator from './TypingIndicator'
import StatusIndicator from './StatusIndicator'
import type { ChatMessage } from '../vite-env.d'
import traceLogoDark from '../assets/trace_logo_dark.svg'

interface MessageListProps {
  messages: ChatMessage[]
  isTyping: boolean
  statusText: string | null
}

export default function MessageList({ messages, isTyping, statusText }: MessageListProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const isNearBottomRef = useRef(true)
  const prevMessagesRef = useRef<ChatMessage[]>(messages)

  // Check if user is near bottom of scroll
  const checkNearBottom = () => {
    if (!containerRef.current) return true
    const { scrollTop, scrollHeight, clientHeight } = containerRef.current
    return scrollHeight - scrollTop - clientHeight < 100
  }

  // Scroll to bottom unconditionally
  const scrollToBottom = () => {
    if (containerRef.current) {
      containerRef.current.scrollTop = containerRef.current.scrollHeight
    }
  }

  // Scroll to bottom if user was near bottom
  const scrollToBottomIfNeeded = () => {
    if (isNearBottomRef.current && containerRef.current) {
      containerRef.current.scrollTop = containerRef.current.scrollHeight
    }
  }

  // Track scroll position
  const handleScroll = () => {
    isNearBottomRef.current = checkNearBottom()
  }

  // Detect tab switch (messages array identity changed) and force scroll to bottom
  useEffect(() => {
    // Check if this is a tab switch by comparing array identity
    // When switching tabs, the messages array is a completely different array
    const isTabSwitch = prevMessagesRef.current !== messages
    
    if (isTabSwitch) {
      // Reset scroll tracking for new tab and force scroll to bottom
      isNearBottomRef.current = true
      // Use requestAnimationFrame to ensure DOM has updated
      requestAnimationFrame(() => {
        scrollToBottom()
      })
    }
    
    prevMessagesRef.current = messages
  }, [messages])

  // Scroll to bottom when messages content changes (not tab switch)
  useEffect(() => {
    scrollToBottomIfNeeded()
  }, [messages, isTyping, statusText])

  return (
    <div 
      className="message-list" 
      ref={containerRef}
      onScroll={handleScroll}
    >
      {messages.length === 0 && !isTyping && (
        <div className="empty-state">
          <div className="empty-state-icon">
            <img src={traceLogoDark} alt="Trace Logo" />
          </div>
          <div className="empty-state-text">Start vibe designing with Trace</div>
          <div className="empty-state-hint">Ask questions about your schematic or request changes</div>
        </div>
      )}
      
      {messages.map((message) => (
        <MessageBubble key={message.id} message={message} />
      ))}
      
      {statusText && <StatusIndicator text={statusText} />}
      
      {isTyping && <TypingIndicator />}
    </div>
  )
}
