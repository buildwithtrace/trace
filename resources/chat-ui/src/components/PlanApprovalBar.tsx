import { useState, useCallback } from 'react'

interface PlanApprovalBarProps {
  onApprove: () => void
  onMoreResearch: (feedback: string) => void
  onAdjust: (feedback: string) => void
  onCancel: () => void
}

export default function PlanApprovalBar({ 
  onApprove, 
  onMoreResearch, 
  onAdjust, 
  onCancel 
}: PlanApprovalBarProps) {
  const [showFeedbackDialog, setShowFeedbackDialog] = useState<'research' | 'adjust' | null>(null)
  const [feedbackText, setFeedbackText] = useState('')

  const handleMoreResearchClick = useCallback(() => {
    setShowFeedbackDialog('research')
    setFeedbackText('')
  }, [])

  const handleAdjustClick = useCallback(() => {
    setShowFeedbackDialog('adjust')
    setFeedbackText('')
  }, [])

  const handleFeedbackSubmit = useCallback(() => {
    if (showFeedbackDialog === 'research') {
      onMoreResearch(feedbackText)
    } else if (showFeedbackDialog === 'adjust') {
      onAdjust(feedbackText)
    }
    setShowFeedbackDialog(null)
    setFeedbackText('')
  }, [showFeedbackDialog, feedbackText, onMoreResearch, onAdjust])

  const handleFeedbackCancel = useCallback(() => {
    setShowFeedbackDialog(null)
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
    <div className="plan-approval-bar">
      {showFeedbackDialog ? (
        <div className="plan-feedback-dialog">
          <div className="plan-feedback-header">
            {showFeedbackDialog === 'research' 
              ? 'What should I research further?' 
              : 'What changes would you like?'}
          </div>
          <textarea
            className="plan-feedback-input"
            value={feedbackText}
            onChange={(e) => setFeedbackText(e.target.value)}
            onKeyDown={handleKeyDown}
            placeholder="Enter your feedback..."
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
            Approve
          </button>
          <button 
            className="plan-button plan-button-neutral"
            onClick={handleMoreResearchClick}
          >
            More Research
          </button>
          <button 
            className="plan-button plan-button-neutral"
            onClick={handleAdjustClick}
          >
            Adjust Plan
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
