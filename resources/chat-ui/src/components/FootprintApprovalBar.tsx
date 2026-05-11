import { useState, useCallback } from 'react'

interface FootprintApprovalBarProps {
  onApprove: () => void
  onModify: (feedback: string) => void
  onCancel: () => void
}

export default function FootprintApprovalBar({
  onApprove,
  onModify,
  onCancel
}: FootprintApprovalBarProps) {
  const [showFeedback, setShowFeedback] = useState(false)
  const [feedbackText, setFeedbackText] = useState('')

  const handleModifyClick = useCallback(() => {
    setShowFeedback(true)
    setFeedbackText('')
  }, [])

  const handleFeedbackSubmit = useCallback(() => {
    onModify(feedbackText)
    setShowFeedback(false)
    setFeedbackText('')
  }, [feedbackText, onModify])

  const handleFeedbackCancel = useCallback(() => {
    setShowFeedback(false)
    setFeedbackText('')
  }, [])

  const handleKeyDown = useCallback((e: React.KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault()
      handleFeedbackSubmit()
    } else if (e.key === 'Escape') {
      handleFeedbackCancel()
    }
  }, [handleFeedbackSubmit, handleFeedbackCancel])

  return (
    <div className="symbol-approval-bar">
      {showFeedback ? (
        <div className="plan-feedback-dialog">
          <div className="plan-feedback-header">
            What changes would you like?
          </div>
          <textarea
            className="plan-feedback-input"
            value={feedbackText}
            onChange={(e) => setFeedbackText(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Describe the modifications (or just type in the chat)..."
            autoFocus
          />
          <div className="plan-feedback-actions">
            <button
              className="plan-feedback-cancel"
              onClick={handleFeedbackCancel}
            >
              Cancel
            </button>
            <button
              className="plan-feedback-submit"
              onClick={handleFeedbackSubmit}
            >
              Submit
            </button>
          </div>
        </div>
      ) : (
        <div className="plan-approval-buttons">
          <button
            className="plan-button plan-button-approve"
            onClick={onApprove}
          >
            Accept
          </button>
          <button
            className="plan-button plan-button-neutral"
            onClick={handleModifyClick}
          >
            Modify
          </button>
          <button
            className="plan-button plan-button-cancel"
            onClick={onCancel}
          >
            Cancel
          </button>
        </div>
      )}
    </div>
  )
}
