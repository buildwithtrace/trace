import { useState, useRef, useEffect, useCallback } from 'react'
import ModeDropdown from './ModeDropdown'
import type { ChatMode, FileAttachment } from '../vite-env.d'

interface InputAreaProps {
  onSendMessage: (content: string, attachments?: FileAttachment[]) => void
  onStopRequest: () => void
  onModeChange: (mode: ChatMode) => void
  mode: ChatMode
  inputEnabled: boolean
  placeholder: string
  isStreaming: boolean
}

export default function InputArea({
  onSendMessage,
  onStopRequest,
  onModeChange,
  mode,
  inputEnabled,
  isStreaming,
}: InputAreaProps) {
  const [inputValue, setInputValue] = useState('')
  const [attachments, setAttachments] = useState<FileAttachment[]>([])
  const [isDragging, setIsDragging] = useState(false)
  const textareaRef = useRef<HTMLTextAreaElement>(null)
  const fileInputRef = useRef<HTMLInputElement>(null)

  // File size limit: 10MB
  const MAX_FILE_SIZE = 10 * 1024 * 1024
  
  // Supported MIME types for Bedrock
  const SUPPORTED_TYPES = [
    'image/png',
    'image/jpeg',
    'image/gif',
    'image/webp',
    'application/pdf'
  ]

  // Auto-resize textarea
  const adjustHeight = useCallback(() => {
    const textarea = textareaRef.current
    if (!textarea) return
    
    // Reset height to auto to get the correct scrollHeight
    textarea.style.height = 'auto'
    
    // Calculate new height (min 24px for single line, max 150px)
    const newHeight = Math.min(Math.max(textarea.scrollHeight, 24), 150)
    textarea.style.height = `${newHeight}px`
  }, [])

  useEffect(() => {
    adjustHeight()
  }, [inputValue, adjustHeight])

  // Process file and convert to base64
  const processFile = useCallback((file: File): Promise<FileAttachment | null> => {
    return new Promise((resolve) => {
      // Validate file type
      if (!SUPPORTED_TYPES.includes(file.type)) {
        alert(`Unsupported file type: ${file.type}. Supported types: images (PNG, JPEG, GIF, WebP) and PDF.`)
        resolve(null)
        return
      }

      // Validate file size
      if (file.size > MAX_FILE_SIZE) {
        alert(`File too large: ${(file.size / 1024 / 1024).toFixed(1)}MB. Maximum size is 10MB.`)
        resolve(null)
        return
      }

      const reader = new FileReader()
      reader.onload = (e) => {
        const result = e.target?.result as string
        if (result) {
          // Extract base64 data (remove data URL prefix)
          const base64Data = result.split(',')[1]
          resolve({
            name: file.name,
            type: file.type,
            data: base64Data,
            size: file.size
          })
        } else {
          resolve(null)
        }
      }
      reader.onerror = () => {
        alert(`Failed to read file: ${file.name}`)
        resolve(null)
      }
      reader.readAsDataURL(file)
    })
  }, [])

  // Handle file selection
  const handleFileSelect = useCallback(async (files: FileList | null) => {
    if (!files || files.length === 0) return

    const processedFiles: FileAttachment[] = []
    for (let i = 0; i < files.length; i++) {
      const processed = await processFile(files[i])
      if (processed) {
        processedFiles.push(processed)
      }
    }

    if (processedFiles.length > 0) {
      setAttachments(prev => [...prev, ...processedFiles])
    }
  }, [processFile])

  // Handle attachment button click
  const handleAttachmentClick = () => {
    fileInputRef.current?.click()
  }

  // Handle file input change
  const handleFileInputChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    handleFileSelect(e.target.files)
    // Reset input to allow selecting the same file again
    e.target.value = ''
  }

  // Remove attachment
  const removeAttachment = (index: number) => {
    setAttachments(prev => prev.filter((_, i) => i !== index))
  }

  // Handle drag and drop
  const handleDragOver = (e: React.DragEvent) => {
    e.preventDefault()
    setIsDragging(true)
  }

  const handleDragLeave = (e: React.DragEvent) => {
    e.preventDefault()
    setIsDragging(false)
  }

  const handleDrop = useCallback(async (e: React.DragEvent) => {
    e.preventDefault()
    setIsDragging(false)
    handleFileSelect(e.dataTransfer.files)
  }, [handleFileSelect])

  // Handle key press
  const handleKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    // Enter to send, Shift+Enter for newline
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      handleSend()
    }
  }

  // Handle send
  const handleSend = () => {
    const trimmed = inputValue.trim()
    if (!trimmed || !inputEnabled) return
    
    onSendMessage(trimmed, attachments.length > 0 ? attachments : undefined)
    setInputValue('')
    setAttachments([])
    
    // Reset textarea height
    if (textareaRef.current) {
      textareaRef.current.style.height = 'auto'
    }
  }

  // Handle stop
  const handleStop = () => {
    onStopRequest()
  }

  const canSend = inputValue.trim() && inputEnabled

  return (
    <div className="input-area">
      {/* Hidden file input */}
      <input
        ref={fileInputRef}
        type="file"
        accept="image/png,image/jpeg,image/gif,image/webp,application/pdf"
        multiple
        style={{ display: 'none' }}
        onChange={handleFileInputChange}
      />

      {/* Unified input box with textarea, mode dropdown, and send button */}
      <div 
        className={`input-box ${isDragging ? 'dragging' : ''}`}
        onDragOver={handleDragOver}
        onDragLeave={handleDragLeave}
        onDrop={handleDrop}
      >
        {/* Attachments preview */}
        {attachments.length > 0 && (
          <div className="attachments-preview">
            {attachments.map((att, index) => (
              <div key={index} className="attachment-chip">
                {att.type.startsWith('image/') && (
                  <div className="attachment-chip-image">
                    <img src={`data:${att.type};base64,${att.data}`} alt={att.name} />
                  </div>
                )}
                <span className="attachment-chip-name">{att.name}</span>
                <button
                  className="attachment-chip-remove"
                  onClick={() => removeAttachment(index)}
                  title="Remove attachment"
                >
                  <svg viewBox="0 0 24 24" width="12" height="12" fill="currentColor">
                    <path d="M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z" />
                  </svg>
                </button>
              </div>
            ))}
          </div>
        )}

        {/* Textarea for input */}
        <textarea
          ref={textareaRef}
          value={inputValue}
          onChange={(e) => setInputValue(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder="Describe what you need..."
          disabled={!inputEnabled}
          rows={1}
        />
        
        {/* Bottom toolbar inside the box */}
        <div className="input-box-toolbar">
          {/* Left side - Attachment button and Mode dropdown */}
          <div className="input-box-left">
            <button
              className="attachment-button"
              onClick={handleAttachmentClick}
              disabled={!inputEnabled}
              title="Attach files (images, PDF)"
            >
              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <path d="M21.44 11.05l-9.19 9.19a6 6 0 0 1-8.49-8.49l9.19-9.19a4 4 0 0 1 5.66 5.66l-9.2 9.19a2 2 0 0 1-2.83-2.83l8.49-8.48" />
              </svg>
            </button>
            <ModeDropdown mode={mode} onModeChange={onModeChange} />
          </div>
          
          {/* Right side - Send/Stop button */}
          <div className="input-box-right">
            {isStreaming ? (
              <button 
                className="send-button stop"
                onClick={handleStop}
                title="Stop generation"
              >
                <svg viewBox="0 0 24 24" width="14" height="14" fill="currentColor">
                  <rect x="6" y="6" width="12" height="12" rx="2" />
                </svg>
              </button>
            ) : (
              <button 
                className={`send-button ${canSend ? 'active' : 'inactive'}`}
                onClick={handleSend}
                disabled={!canSend}
                title="Send message"
              >
                <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
                  <path d="M12 19V5M5 12l7-7 7 7" />
                </svg>
              </button>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}
