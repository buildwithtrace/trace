import { useState } from 'react'

interface FileEditWidgetProps {
  filename: string
  oldContent: string
  newContent: string
  visible: boolean
}

function computeDiff(oldContent: string, newContent: string): DiffLine[] {
  const oldLines = oldContent.split('\n')
  const newLines = newContent.split('\n')
  const result: DiffLine[] = []

  const maxLen = Math.max(oldLines.length, newLines.length)
  let oi = 0
  let ni = 0

  while (oi < oldLines.length || ni < newLines.length) {
    if (oi < oldLines.length && ni < newLines.length && oldLines[oi] === newLines[ni]) {
      result.push({ type: 'context', content: oldLines[oi] })
      oi++
      ni++
    } else {
      // Look ahead to find next matching line
      let foundMatch = false
      const lookAhead = Math.min(10, maxLen)

      for (let delta = 1; delta <= lookAhead && !foundMatch; delta++) {
        if (oi + delta < oldLines.length && ni < newLines.length && oldLines[oi + delta] === newLines[ni]) {
          for (let r = 0; r < delta; r++) {
            result.push({ type: 'remove', content: oldLines[oi + r] })
          }
          oi += delta
          foundMatch = true
        } else if (ni + delta < newLines.length && oi < oldLines.length && newLines[ni + delta] === oldLines[oi]) {
          for (let a = 0; a < delta; a++) {
            result.push({ type: 'add', content: newLines[ni + a] })
          }
          ni += delta
          foundMatch = true
        }
      }

      if (!foundMatch) {
        if (oi < oldLines.length) {
          result.push({ type: 'remove', content: oldLines[oi] })
          oi++
        }
        if (ni < newLines.length) {
          result.push({ type: 'add', content: newLines[ni] })
          ni++
        }
      }
    }
  }

  return result
}

interface DiffLine {
  type: 'add' | 'remove' | 'context'
  content: string
}

export default function FileEditWidget({ filename, oldContent, newContent, visible }: FileEditWidgetProps) {
  const [expanded, setExpanded] = useState(true)

  if (!visible) return null

  const diffLines = computeDiff(oldContent, newContent)

  return (
    <div className="file-edit-widget">
      <button className="file-edit-header" onClick={() => setExpanded(!expanded)}>
        <svg
          className={`file-edit-chevron ${expanded ? 'expanded' : ''}`}
          width="12"
          height="12"
          viewBox="0 0 12 12"
          fill="none"
          stroke="currentColor"
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
        >
          <path d="M4 2l4 4-4 4" />
        </svg>
        <span className="file-edit-filename">{filename}</span>
      </button>
      {expanded && (
        <div className="file-edit-diff">
          {diffLines.map((line, i) => {
            const cls =
              line.type === 'add' ? 'diff-add' :
              line.type === 'remove' ? 'diff-remove' :
              'diff-context'
            const prefix =
              line.type === 'add' ? '+' :
              line.type === 'remove' ? '-' :
              ' '
            return (
              <div key={i} className={cls}>
                <span className="diff-prefix">{prefix}</span>
                <span>{line.content}</span>
              </div>
            )
          })}
        </div>
      )}
    </div>
  )
}
