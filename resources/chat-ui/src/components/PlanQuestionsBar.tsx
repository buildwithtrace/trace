import { useState, useCallback, useMemo } from 'react'
import type { StructuredQuestion } from '../vite-env.d'

interface PlanQuestionsBarProps {
  questions: StructuredQuestion[]
  onSubmit: (answers: Record<string, any>) => void
}

type Answer = { option?: string; custom_text?: string; text?: string }
type AnswerMap = Map<number, Answer>

export default function PlanQuestionsBar({ questions, onSubmit }: PlanQuestionsBarProps) {
  const [answers, setAnswers] = useState<AnswerMap>(new Map())
  const [showCustom, setShowCustom] = useState<Set<number>>(new Set())

  const handleOptionClick = useCallback((questionId: number, optionId: string, isCustom?: boolean) => {
    setAnswers(prev => {
      const next = new Map(prev)
      if (isCustom) {
        next.set(questionId, { option: optionId, custom_text: prev.get(questionId)?.custom_text ?? '' })
      } else {
        next.set(questionId, { option: optionId })
      }
      return next
    })
    setShowCustom(prev => {
      const next = new Set(prev)
      if (isCustom) {
        next.add(questionId)
      } else {
        next.delete(questionId)
      }
      return next
    })
  }, [])

  const handleCustomTextChange = useCallback((questionId: number, text: string) => {
    setAnswers(prev => {
      const next = new Map(prev)
      const existing = prev.get(questionId) ?? {}
      next.set(questionId, { ...existing, custom_text: text })
      return next
    })
  }, [])

  const handleFreeformChange = useCallback((questionId: number, text: string) => {
    setAnswers(prev => {
      const next = new Map(prev)
      next.set(questionId, { text })
      return next
    })
  }, [])

  const allAnswered = useMemo(() => {
    return questions.every(q => {
      const a = answers.get(q.id)
      if (!a) return false
      if (q.is_freeform) return !!a.text?.trim()
      if (a.option) {
        const isCustomOpt = q.options.find(o => o.id === a.option)?.is_custom
        if (isCustomOpt) return !!a.custom_text?.trim()
        return true
      }
      return false
    })
  }, [questions, answers])

  const handleSubmit = useCallback(() => {
    if (!allAnswered) return
    const payload: Record<string, any> = {}
    answers.forEach((val, key) => {
      payload[String(key)] = val
    })
    onSubmit(payload)
  }, [allAnswered, answers, onSubmit])

  return (
    <div className="plan-questions-bar">
      <div className="plan-questions-scroll">
        {questions.map(q => (
          <div key={q.id} className="plan-question-block">
            <div className="plan-question-heading">
              {q.id}. {q.text}
            </div>
            {q.is_freeform ? (
              <textarea
                className="plan-question-freeform"
                value={answers.get(q.id)?.text ?? ''}
                onChange={e => handleFreeformChange(q.id, e.target.value)}
                placeholder="Type your answer here..."
              />
            ) : (
              <>
                <div className="plan-question-options">
                  {q.options.map(opt => (
                    <button
                      key={opt.id}
                      className={
                        'plan-question-pill'
                        + (answers.get(q.id)?.option === opt.id ? ' selected' : '')
                        + (opt.is_custom ? ' is-custom' : '')
                      }
                      onClick={() => handleOptionClick(q.id, opt.id, opt.is_custom)}
                    >
                      <span className="plan-question-pill-letter">{opt.id})</span> {opt.label}
                    </button>
                  ))}
                </div>
                {showCustom.has(q.id) && (
                  <input
                    type="text"
                    className="plan-question-custom-input"
                    value={answers.get(q.id)?.custom_text ?? ''}
                    onChange={e => handleCustomTextChange(q.id, e.target.value)}
                    placeholder="Please specify..."
                    autoFocus
                  />
                )}
              </>
            )}
          </div>
        ))}
      </div>
      <div className="plan-questions-footer">
        <button
          className={'plan-button plan-button-approve' + (!allAnswered ? ' disabled' : '')}
          disabled={!allAnswered}
          onClick={handleSubmit}
        >
          Submit Answers
        </button>
      </div>
    </div>
  )
}
