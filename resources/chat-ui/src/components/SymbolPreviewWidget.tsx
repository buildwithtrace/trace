import { useState, useMemo } from 'react'
import type { SymbolPreview } from '../vite-env.d'

interface Props {
  symbolPreview: SymbolPreview
}

interface PinDef {
  name: string
  electrical_type: string
  graphic_style?: string
  hidden?: boolean
}

interface PinGroupDef {
  name?: string
  pins: string[]
  spacing_mm?: number
  pin_length_mm?: number
}

interface SideDef {
  groups: PinGroupDef[]
  group_spacing_mm?: number
  alignment?: string
}

interface SidesDef {
  left?: SideDef
  right?: SideDef
  top?: SideDef
  bottom?: SideDef
}

interface UnitDef {
  id: number
  name?: string
  is_shared?: boolean
  sides: SidesDef
}

interface BodyDef {
  fill?: string
  stroke_width_mm?: number
  min_width_mm?: number | null
  min_height_mm?: number | null
}

// --- Layout engine constants (matching layout_engine.py) ---

const GRID = 2.54
const PIN_NAME_CHAR_WIDTH_MM = 1.27
const TEXT_SIZE = 1.27
const INVERTED_CIRCLE_R = 0.508

// --- KiCad-like colors ---

const BODY_FILL = '#FFFFDE'
const BODY_STROKE = '#8B0000'
const PIN_LINE_COLOR = '#008484'
const PIN_NAME_COLOR = '#008484'
const PIN_NUM_COLOR = '#CC0000'

// --- Pin type colors (kept from original for subtle indicators) ---

function typeColor(t: string): string {
  const map: Record<string, string> = {
    power_in: '#e74c3c', power_out: '#e74c3c',
    input: '#3498db', output: '#2ecc71',
    bidirectional: '#f39c12', passive: '#95a5a6',
    no_connect: '#7f8c8d',
  }
  return map[t] ?? '#999999'
}

const PIN_TYPE_LEGEND: Array<{ type: string; color: string; label: string }> = [
  { type: 'power_in', color: '#e74c3c', label: 'Power' },
  { type: 'power_out', color: '#e74c3c', label: 'Power Out' },
  { type: 'input', color: '#3498db', label: 'Input' },
  { type: 'output', color: '#2ecc71', label: 'Output' },
  { type: 'bidirectional', color: '#f39c12', label: 'Bidirectional' },
  { type: 'passive', color: '#95a5a6', label: 'Passive' },
  { type: 'tri_state', color: '#9b59b6', label: 'Tri-State' },
  { type: 'open_collector', color: '#e67e22', label: 'Open Collector' },
  { type: 'open_emitter', color: '#e67e22', label: 'Open Emitter' },
  { type: 'no_connect', color: '#7f8c8d', label: 'No Connect' },
  { type: 'unspecified', color: '#999999', label: 'Unspecified' },
]

// --- Layout engine math (ported from layout_engine.py) ---

function snapUp(value: number, grid: number = GRID): number {
  const steps = Math.ceil(value / grid - 1e-9)
  return Math.max(grid, steps * grid)
}

function sideSpan(side: SideDef): number {
  let span = 0
  for (let i = 0; i < side.groups.length; i++) {
    const grp = side.groups[i]
    const spacing = grp.spacing_mm ?? GRID
    const pinCount = grp.pins.length
    span += (pinCount - 1) * spacing
    if (i > 0) {
      span += side.group_spacing_mm ?? 5.08
    }
  }
  return span
}

function cleanPinName(name: string): string {
  if (name === '~') return ''
  return name.replace(/~\{([^}]+)\}/g, '$1')
}

function maxPinNameWidth(side: SideDef | undefined, pins: Record<string, PinDef>): number {
  if (!side) return 0
  let maxW = 0
  for (const grp of side.groups) {
    for (const pn of grp.pins) {
      const p = pins[pn]
      if (p) {
        const name = cleanPinName(p.name)
        maxW = Math.max(maxW, name.length * PIN_NAME_CHAR_WIDTH_MM)
      }
    }
  }
  return maxW
}

function computeBodySize(
  unit: UnitDef,
  pins: Record<string, PinDef>,
  body: BodyDef,
  pinNameOffset: number
): [number, number] {
  const leftSpan = unit.sides.left ? sideSpan(unit.sides.left) : 0
  const rightSpan = unit.sides.right ? sideSpan(unit.sides.right) : 0
  const topSpan = unit.sides.top ? sideSpan(unit.sides.top) : 0
  const bottomSpan = unit.sides.bottom ? sideSpan(unit.sides.bottom) : 0

  const leftNameW = maxPinNameWidth(unit.sides.left, pins)
  const rightNameW = maxPinNameWidth(unit.sides.right, pins)

  const padding = pinNameOffset * 2 + GRID
  let autoWidth = leftNameW + rightNameW + padding
  autoWidth = Math.max(autoWidth, topSpan + GRID * 2)
  autoWidth = Math.max(autoWidth, bottomSpan + GRID * 2)

  const autoHeight = Math.max(leftSpan, rightSpan) + GRID * 2

  const width = snapUp(Math.max(autoWidth, body.min_width_mm ?? 0))
  const height = snapUp(Math.max(autoHeight, body.min_height_mm ?? 0))
  return [width, height]
}

function alignOffset(totalSpan: number, bodyExtent: number, alignment: string): number {
  const margin = GRID
  const available = bodyExtent - 2 * margin

  if (alignment === 'top' || alignment === 'left') {
    return available / 2
  }
  if (alignment === 'bottom' || alignment === 'right') {
    return -(available / 2) + totalSpan
  }
  return totalSpan / 2
}

function pinPositionsLR(side: SideDef, bodyHalfHeight: number): number[] {
  const total = sideSpan(side)
  const startY = alignOffset(total, bodyHalfHeight * 2, side.alignment ?? 'center')

  const positions: number[] = []
  let cursor = startY
  for (let gi = 0; gi < side.groups.length; gi++) {
    const grp = side.groups[gi]
    if (gi > 0) {
      cursor -= (side.group_spacing_mm ?? 5.08)
    }
    for (let pi = 0; pi < grp.pins.length; pi++) {
      if (pi > 0) {
        cursor -= (grp.spacing_mm ?? GRID)
      }
      positions.push(cursor)
    }
  }
  return positions
}

function pinPositionsTB(side: SideDef, bodyHalfWidth: number): number[] {
  const total = sideSpan(side)
  const startX = -alignOffset(total, bodyHalfWidth * 2, side.alignment ?? 'center')

  const positions: number[] = []
  let cursor = startX
  for (let gi = 0; gi < side.groups.length; gi++) {
    const grp = side.groups[gi]
    if (gi > 0) {
      cursor += (side.group_spacing_mm ?? 5.08)
    }
    for (let pi = 0; pi < grp.pins.length; pi++) {
      if (pi > 0) {
        cursor += (grp.spacing_mm ?? GRID)
      }
      positions.push(cursor)
    }
  }
  return positions
}

// Compute max pin length on a side
function maxPinLength(side: SideDef | undefined): number {
  if (!side) return 0
  let maxLen = 0
  for (const grp of side.groups) {
    maxLen = Math.max(maxLen, grp.pin_length_mm ?? GRID)
  }
  return maxLen
}

// --- Resolved pin data for rendering ---

interface ResolvedPin {
  pinNum: string
  pinDef: PinDef
  name: string
  side: 'left' | 'right' | 'top' | 'bottom'
  // endpoint (tip of pin, away from body)
  ex: number
  ey: number
  // body connection point
  bx: number
  by: number
  pinLength: number
}

function resolveUnitPins(
  unit: UnitDef,
  pins: Record<string, PinDef>,
  hw: number,
  hh: number,
): ResolvedPin[] {
  const resolved: ResolvedPin[] = []

  function emitSide(
    side: SideDef | undefined,
    positions: number[],
    sideName: 'left' | 'right' | 'top' | 'bottom',
  ) {
    if (!side) return
    let pinIdx = 0
    for (const grp of side.groups) {
      const length = grp.pin_length_mm ?? GRID
      for (const pn of grp.pins) {
        const p = pins[pn]
        if (!p || p.hidden) { pinIdx++; continue }
        const pos = positions[pinIdx]

        let ex: number, ey: number, bx: number, by: number
        if (sideName === 'left') {
          ex = -hw - length; ey = pos
          bx = -hw; by = pos
        } else if (sideName === 'right') {
          ex = hw + length; ey = pos
          bx = hw; by = pos
        } else if (sideName === 'top') {
          ex = pos; ey = hh + length
          bx = pos; by = hh
        } else {
          ex = pos; ey = -(hh + length)
          bx = pos; by = -hh
        }

        resolved.push({
          pinNum: pn,
          pinDef: p,
          name: cleanPinName(p.name),
          side: sideName,
          ex, ey, bx, by,
          pinLength: length,
        })
        pinIdx++
      }
    }
  }

  if (unit.sides.left) {
    const positions = pinPositionsLR(unit.sides.left, hh)
    emitSide(unit.sides.left, positions, 'left')
  }
  if (unit.sides.right) {
    const positions = pinPositionsLR(unit.sides.right, hh)
    emitSide(unit.sides.right, positions, 'right')
  }
  if (unit.sides.top) {
    const positions = pinPositionsTB(unit.sides.top, hw)
    emitSide(unit.sides.top, positions, 'top')
  }
  if (unit.sides.bottom) {
    const positions = pinPositionsTB(unit.sides.bottom, hw)
    emitSide(unit.sides.bottom, positions, 'bottom')
  }

  return resolved
}

// --- SVG sub-components ---

function PinGraphicStyle({ pin, side, bx, by }: {
  pin: ResolvedPin
  side: 'left' | 'right' | 'top' | 'bottom'
  bx: number
  by: number
}) {
  const style = pin.pinDef.graphic_style ?? 'line'
  if (style === 'line' || style === 'non_logic') return null

  const r = INVERTED_CIRCLE_R
  const chevronSize = 0.6

  if (style === 'inverted' || style === 'inverted_clock') {
    let cx = bx, cy = by
    if (side === 'left') cx = bx - r
    else if (side === 'right') cx = bx + r
    else if (side === 'top') cy = by + r
    else cy = by - r

    return (
      <g>
        <circle cx={cx} cy={-cy} r={r} fill={BODY_FILL} stroke={PIN_LINE_COLOR} strokeWidth={0.15} />
        {(style === 'inverted_clock') && <ClockChevron side={side} bx={bx} by={by} size={chevronSize} />}
      </g>
    )
  }

  if (style === 'clock' || style === 'clock_low' || style === 'edge_clock_high') {
    return <ClockChevron side={side} bx={bx} by={by} size={chevronSize} />
  }

  if (style === 'input_low') {
    const s = 0.6
    if (side === 'left' || side === 'right') {
      const dir = side === 'left' ? 1 : -1
      return <line x1={bx} y1={-by} x2={bx + dir * s} y2={-(by - s)} stroke={PIN_LINE_COLOR} strokeWidth={0.15} />
    }
    const dir = side === 'top' ? -1 : 1
    return <line x1={bx} y1={-by} x2={bx - s} y2={-(by + dir * s)} stroke={PIN_LINE_COLOR} strokeWidth={0.15} />
  }

  if (style === 'output_low') {
    const s = 0.6
    if (side === 'left' || side === 'right') {
      const dir = side === 'left' ? -1 : 1
      return <line x1={bx} y1={-by} x2={bx + dir * s} y2={-(by + s)} stroke={PIN_LINE_COLOR} strokeWidth={0.15} />
    }
  }

  return null
}

function ClockChevron({ side, bx, by, size }: { side: string; bx: number; by: number; size: number }) {
  // KiCad clock chevron points INWARD into the body
  let points: string
  if (side === 'left') {
    // Pin comes from left, chevron ">" points right (into body)
    points = `${bx},${-(by + size)} ${bx + size},${-by} ${bx},${-(by - size)}`
  } else if (side === 'right') {
    // Pin comes from right, chevron "<" points left (into body)
    points = `${bx},${-(by + size)} ${bx - size},${-by} ${bx},${-(by - size)}`
  } else if (side === 'top') {
    // Pin comes from top, chevron "v" points down (into body)
    points = `${bx - size},${-by} ${bx},${-(by - size)} ${bx + size},${-by}`
  } else {
    // Pin comes from bottom, chevron "^" points up (into body)
    points = `${bx - size},${-by} ${bx},${-(by + size)} ${bx + size},${-by}`
  }
  return <polyline points={points} fill="none" stroke={BODY_STROKE} strokeWidth={0.15} />
}

// --- Main component ---

export default function SymbolPreviewWidget({ symbolPreview }: Props) {
  const [showJson, setShowJson] = useState(false)
  const sym = symbolPreview.symbolJson as Record<string, unknown>

  const name = (sym.name as string) ?? 'Unknown'
  const description = (sym.description as string) ?? ''
  const refPrefix = (sym.reference_prefix as string) ?? 'U'
  const units = (sym.units as UnitDef[]) ?? []
  const pins = (sym.pins as Record<string, PinDef>) ?? {}
  const totalPins = Object.keys(pins).length
  const isPower = (sym.is_power_symbol as boolean) ?? false
  const pinNameOffset = (sym.pin_name_offset_mm as number) ?? 1.016
  const pinNamesVisible = (sym.pin_names_visible as boolean) ?? true
  const pinNumbersVisible = (sym.pin_numbers_visible as boolean) ?? true
  const body: BodyDef = (sym.body as BodyDef) ?? {}

  const usedTypes = useMemo(() => {
    const types = new Set<string>()
    for (const p of Object.values(pins)) {
      types.add(p.electrical_type)
    }
    return PIN_TYPE_LEGEND.filter(e => types.has(e.type))
  }, [pins])

  return (
    <div className="symbol-preview-widget">
      <div className="symbol-preview-header">
        <div className="symbol-preview-header-top">
          <div>
            <div className="symbol-preview-title">
              <span className="symbol-preview-ref">{refPrefix}</span>
              <span className="symbol-preview-name">{name}</span>
              {isPower && <span className="symbol-preview-badge">PWR</span>}
            </div>
            <div className="symbol-preview-meta">
              {totalPins} pin{totalPins !== 1 ? 's' : ''} · {units.length} unit{units.length !== 1 ? 's' : ''}
            </div>
          </div>
          {usedTypes.length > 0 && (
            <div className="symbol-pin-legend">
              {usedTypes.map(e => (
                <div key={e.type} className="symbol-pin-legend-item">
                  <span className="symbol-pin-legend-dot" style={{ background: e.color }} />
                  <span>{e.label}</span>
                </div>
              ))}
            </div>
          )}
        </div>
        {description && <div className="symbol-preview-desc">{description}</div>}
      </div>

      {units.map(unit => (
        <UnitDiagram
          key={unit.id}
          unit={unit}
          pins={pins}
          unitCount={units.length}
          body={body}
          pinNameOffset={pinNameOffset}
          pinNamesVisible={pinNamesVisible}
          pinNumbersVisible={pinNumbersVisible}
          allUnits={units}
        />
      ))}

      <button
        className="symbol-preview-json-toggle"
        onClick={() => setShowJson(!showJson)}
      >
        {showJson ? 'Hide JSON' : 'View JSON'}
      </button>

      {showJson && (
        <pre className="symbol-preview-json">
          {JSON.stringify(sym, null, 2)}
        </pre>
      )}
    </div>
  )
}

function UnitDiagram({ unit, pins, unitCount, body, pinNameOffset, pinNamesVisible, pinNumbersVisible, allUnits }: {
  unit: UnitDef
  pins: Record<string, PinDef>
  unitCount: number
  body: BodyDef
  pinNameOffset: number
  pinNamesVisible: boolean
  pinNumbersVisible: boolean
  allUnits: UnitDef[]
}) {
  const layout = useMemo(() => {
    let maxW = 0, maxH = 0
    for (const u of allUnits) {
      const [w, h] = computeBodySize(u, pins, body, pinNameOffset)
      maxW = Math.max(maxW, w)
      maxH = Math.max(maxH, h)
    }
    const width = snapUp(maxW)
    const height = snapUp(maxH)
    const hw = width / 2
    const hh = height / 2

    const resolvedPins = resolveUnitPins(unit, pins, hw, hh)

    const leftPinLen = maxPinLength(unit.sides.left)
    const rightPinLen = maxPinLength(unit.sides.right)
    const topPinLen = maxPinLength(unit.sides.top)
    const bottomPinLen = maxPinLength(unit.sides.bottom)

    // Margin for pin number text beyond the endpoint
    const numTextMargin = 1.5

    let xMin = -hw - leftPinLen - numTextMargin
    let xMax = hw + rightPinLen + numTextMargin
    let yMin = -(hh + topPinLen + numTextMargin)
    let yMax = hh + bottomPinLen + numTextMargin

    const pad = 1.0
    xMin -= pad; xMax += pad
    yMin -= pad; yMax += pad

    const vbWidth = xMax - xMin
    const vbHeight = yMax - yMin

    const clipId = `body-clip-${unit.id}`

    return { width, height, hw, hh, resolvedPins, xMin, yMin, vbWidth, vbHeight, clipId }
  }, [unit, pins, body, pinNameOffset, allUnits])

  const { width: bodyW, height: bodyH, hw, hh, resolvedPins, xMin, yMin, vbWidth, vbHeight, clipId } = layout

  const strokeW = body.stroke_width_mm ?? 0.254
  const fillType = body.fill ?? 'background'
  const bodyFillColor = fillType === 'none' ? 'none' : (fillType === 'outline' ? BODY_STROKE : BODY_FILL)

  const containerWidth = 300
  const scale = containerWidth / vbWidth
  const svgHeight = vbHeight * scale

  return (
    <div className="symbol-unit">
      {unitCount > 1 && (
        <div className="symbol-unit-label">Unit {unit.id}{unit.name ? `: ${unit.name}` : ''}</div>
      )}
      <div className="symbol-diagram-svg-container">
        <svg
          width={containerWidth}
          height={svgHeight}
          viewBox={`${xMin} ${yMin} ${vbWidth} ${vbHeight}`}
          className="symbol-diagram-svg"
        >
          <defs>
            <clipPath id={clipId}>
              <rect x={-hw} y={-hh} width={bodyW} height={bodyH} />
            </clipPath>
          </defs>

          <rect
            x={-hw}
            y={-hh}
            width={bodyW}
            height={bodyH}
            fill={bodyFillColor}
            stroke={BODY_STROKE}
            strokeWidth={strokeW}
          />

          {resolvedPins.map(pin => (
            <PinSvg
              key={`${pin.side}-${pin.pinNum}`}
              pin={pin}
              pinNameOffset={pinNameOffset}
              pinNamesVisible={pinNamesVisible}
              pinNumbersVisible={pinNumbersVisible}
              hw={hw}
              hh={hh}
              bodyClipId={clipId}
            />
          ))}
        </svg>
      </div>
    </div>
  )
}

function PinSvg({ pin, pinNameOffset, pinNamesVisible, pinNumbersVisible, hw, hh, bodyClipId }: {
  pin: ResolvedPin
  pinNameOffset: number
  pinNamesVisible: boolean
  pinNumbersVisible: boolean
  hw: number
  hh: number
  bodyClipId: string
}) {
  const { pinNum, pinDef, name, side, ex, ey, bx, by } = pin
  const color = typeColor(pinDef.electrical_type)

  const endpointR = 0.2

  const gStyle = pinDef.graphic_style ?? 'line'
  const isInverted = gStyle === 'inverted' || gStyle === 'inverted_clock'
  const invertOffset = isInverted ? INVERTED_CIRCLE_R * 2 : 0

  let lineEx = ex, lineEy = ey
  let lineBx = bx, lineBy = by
  if (isInverted) {
    if (side === 'left') lineBx = bx - invertOffset
    else if (side === 'right') lineBx = bx + invertOffset
    else if (side === 'top') lineBy = by + invertOffset
    else lineBy = by - invertOffset
  }

  // Pin number: placed at the midpoint of the pin stub, offset perpendicular
  type Anchor = 'middle' | 'start' | 'end'
  const midX = (ex + bx) / 2
  const midY = (ey + by) / 2
  const numPerpOffset = 0.6

  let numX = midX, numY = midY, numAnchor: Anchor = 'middle', numRotation = 0
  if (side === 'left') {
    numX = midX; numY = midY + numPerpOffset; numAnchor = 'middle'
  } else if (side === 'right') {
    numX = midX; numY = midY + numPerpOffset; numAnchor = 'middle'
  } else if (side === 'top') {
    numX = midX - numPerpOffset; numY = midY; numAnchor = 'middle'; numRotation = 90
  } else {
    numX = midX - numPerpOffset; numY = midY; numAnchor = 'middle'; numRotation = 90
  }

  // Pin name: inside body, offset from body edge
  const nameOff = pinNameOffset
  let nameX = 0, nameY = 0, nameAnchor: Anchor = 'start', nameRotation = 0
  if (side === 'left') {
    nameX = -hw + nameOff; nameY = by; nameAnchor = 'start'
  } else if (side === 'right') {
    nameX = hw - nameOff; nameY = by; nameAnchor = 'end'
  } else if (side === 'top') {
    nameX = bx; nameY = hh - nameOff; nameAnchor = 'end'; nameRotation = 90
  } else {
    nameX = bx; nameY = -(hh - nameOff); nameAnchor = 'start'; nameRotation = 90
  }

  return (
    <g>
      <line
        x1={lineEx} y1={-lineEy}
        x2={lineBx} y2={-lineBy}
        stroke={color}
        strokeWidth={0.15}
      />

      <circle cx={ex} cy={-ey} r={endpointR} fill="none" stroke={color} strokeWidth={0.1} />

      <PinGraphicStyle pin={pin} side={side} bx={bx} by={by} />

      {pinNumbersVisible && (
        <text
          x={numX}
          y={-numY}
          textAnchor={numAnchor}
          dominantBaseline="auto"
          fill={PIN_NUM_COLOR}
          fontSize={TEXT_SIZE}
          fontFamily="sans-serif"
          transform={numRotation ? `rotate(${-numRotation}, ${numX}, ${-numY})` : undefined}
        >
          {pinNum}
        </text>
      )}

      {pinNamesVisible && name && (
        <g clipPath={`url(#${bodyClipId})`}>
          <text
            x={nameX}
            y={-nameY}
            textAnchor={nameAnchor}
            dominantBaseline="central"
            fill={PIN_NAME_COLOR}
            fontSize={TEXT_SIZE}
            fontFamily="sans-serif"
            fontWeight={500}
            transform={nameRotation ? `rotate(${-nameRotation}, ${nameX}, ${-nameY})` : undefined}
          >
            {name}
          </text>
        </g>
      )}
    </g>
  )
}
