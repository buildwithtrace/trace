import { useState, useMemo } from 'react'
import type { FootprintPreview } from '../vite-env.d'

interface Props {
  footprintPreview: FootprintPreview
}

interface PadDef {
  number: string
  function?: string
  x_mm: number
  y_mm: number
  shape: { type: string; width_mm?: number; height_mm?: number; diameter_mm?: number }
  drill?: { diameter_mm: number } | null
}

interface BodyDef {
  width_mm: number
  height_mm: number
  pin1_chamfer_mm?: number
}

interface ThermalPadDef {
  number: string
  x_mm?: number
  y_mm?: number
  shape: { type: string; width_mm?: number; height_mm?: number; diameter_mm?: number }
}

interface DimAnnotation {
  type: 'horizontal' | 'vertical'
  ax: number; ay: number
  bx: number; by: number
  offset: number
  label: string
}

interface SilkSegment {
  x1: number; y1: number; x2: number; y2: number
}

interface CourtyardRect {
  x: number; y: number; w: number; h: number
}

function padWidth(shape: PadDef['shape']): number {
  if (shape.diameter_mm) return shape.diameter_mm
  return shape.width_mm ?? 0.5
}

function padHeight(shape: PadDef['shape']): number {
  if (shape.diameter_mm) return shape.diameter_mm
  return shape.height_mm ?? 0.5
}

function fmtMm(v: number): string {
  const s = v.toFixed(2).replace(/\.?0+$/, '')
  return `${s}mm`
}

function roundClose(a: number, b: number, tol = 0.01): boolean {
  return Math.abs(a - b) < tol
}

function snapCourtyardOut(v: number): number {
  return v >= 0 ? Math.ceil(v * 100) / 100 : Math.floor(v * 100) / 100
}

export default function FootprintPreviewWidget({ footprintPreview }: Props) {
  const [showJson, setShowJson] = useState(false)
  const fp = footprintPreview.footprintJson as Record<string, unknown>

  const name = (fp.name as string) ?? 'Unknown'
  const description = (fp.description as string) ?? ''
  const attr = (fp.attr as string) ?? 'smd'
  const pads = (fp.pads as PadDef[]) ?? []
  const body = (fp.body as BodyDef) ?? { width_mm: 2, height_mm: 2 }
  const thermalPad = fp.thermal_pad as ThermalPadDef | null | undefined
  const courtyardMargin = (fp.courtyard_margin_mm as number) ?? 0.25
  const pin1Chamfer = body.pin1_chamfer_mm ?? 0.3

  const totalPads = pads.length + (thermalPad ? 1 : 0)

  const { viewBox, scale, allPads, dims, layers } = useMemo(() => {
    const allP: Array<{ num: string; fn: string; x: number; y: number; w: number; h: number; isPin1: boolean; isCircle: boolean; isThermal: boolean }> = []

    for (const pad of pads) {
      allP.push({
        num: pad.number,
        fn: pad.function ?? '',
        x: pad.x_mm,
        y: pad.y_mm,
        w: padWidth(pad.shape),
        h: padHeight(pad.shape),
        isPin1: pad.number === '1' || pad.number === 'A1',
        isCircle: pad.shape.type === 'circle',
        isThermal: false,
      })
    }

    if (thermalPad) {
      allP.push({
        num: thermalPad.number,
        fn: 'EP',
        x: thermalPad.x_mm ?? 0,
        y: thermalPad.y_mm ?? 0,
        w: padWidth(thermalPad.shape),
        h: padHeight(thermalPad.shape),
        isPin1: false,
        isCircle: thermalPad.shape.type === 'circle',
        isThermal: true,
      })
    }

    const bw = body.width_mm
    const bh = body.height_mm
    const halfW = bw / 2
    const halfH = bh / 2

    // --- F.Fab body outline with pin-1 chamfer ---
    const chamfer = Math.min(pin1Chamfer, halfW, halfH)
    const fabPath = `M ${-halfW + chamfer} ${-halfH} L ${halfW} ${-halfH} L ${halfW} ${halfH} L ${-halfW} ${halfH} L ${-halfW} ${-halfH + chamfer} Z`

    // --- Silkscreen: body offset outward, clipped around pads ---
    const SILK_OFFSET = 0.11
    const SILK_CLEARANCE = 0.2
    const sx = halfW + SILK_OFFSET
    const sy = halfH + SILK_OFFSET

    const padBoxes: Array<[number, number, number, number]> = []
    for (const p of allP) {
      padBoxes.push([
        p.x - p.w / 2 - SILK_CLEARANCE,
        p.y - p.h / 2 - SILK_CLEARANCE,
        p.x + p.w / 2 + SILK_CLEARANCE,
        p.y + p.h / 2 + SILK_CLEARANCE,
      ])
    }

    function segOverlapsPads(x1: number, y1: number, x2: number, y2: number): boolean {
      const sxMin = Math.min(x1, x2), sxMax = Math.max(x1, x2)
      const syMin = Math.min(y1, y2), syMax = Math.max(y1, y2)
      for (const [bx0, by0, bx1, by1] of padBoxes) {
        if (sxMin < bx1 && sxMax > bx0 && syMin < by1 && syMax > by0) return true
      }
      return false
    }

    const silkSegments: SilkSegment[] = []
    const rawSilkEdges: Array<[number, number, number, number]> = [
      [-sx, -sy, -sx, sy],
      [-sx, sy, sx, sy],
      [sx, sy, sx, -sy],
      [sx, -sy, -sx, -sy],
    ]
    for (const [x1, y1, x2, y2] of rawSilkEdges) {
      if (!segOverlapsPads(x1, y1, x2, y2)) {
        silkSegments.push({ x1, y1, x2, y2 })
      }
    }

    // Pin-1 silk triangle
    const triSize = 0.28
    const triX = -sx - triSize * 0.5
    const triY = -sy
    const silkTrianglePath = `M ${triX + triSize} ${triY} L ${triX} ${triY} L ${triX + triSize} ${triY - triSize} Z`

    // --- Courtyard: bounding box of pads + body + margin ---
    let cxMin = -halfW, cyMin = -halfH, cxMax = halfW, cyMax = halfH
    for (const p of allP) {
      cxMin = Math.min(cxMin, p.x - p.w / 2)
      cyMin = Math.min(cyMin, p.y - p.h / 2)
      cxMax = Math.max(cxMax, p.x + p.w / 2)
      cyMax = Math.max(cyMax, p.y + p.h / 2)
    }
    const courtyard: CourtyardRect = {
      x: snapCourtyardOut(cxMin - courtyardMargin),
      y: snapCourtyardOut(cyMin - courtyardMargin),
      w: snapCourtyardOut(cxMax + courtyardMargin) - snapCourtyardOut(cxMin - courtyardMargin),
      h: snapCourtyardOut(cyMax + courtyardMargin) - snapCourtyardOut(cyMin - courtyardMargin),
    }

    // --- Compute dimensions ---
    const annotations: DimAnnotation[] = []
    const signalPads = allP.filter(p => !p.isThermal)

    const colMap = new Map<number, typeof signalPads>()
    for (const p of signalPads) {
      const rx = Math.round(p.x * 100) / 100
      let found = false
      for (const [key, arr] of colMap) {
        if (roundClose(rx, key)) { arr.push(p); found = true; break }
      }
      if (!found) colMap.set(rx, [p])
    }
    const columns = [...colMap.entries()].sort((a, b) => a[0] - b[0])

    let pitch: number | null = null
    let pitchCol: typeof signalPads | null = null
    for (const [, col] of columns) {
      if (col.length >= 2) {
        const sorted = [...col].sort((a, b) => a.y - b.y)
        const gaps: number[] = []
        for (let i = 1; i < sorted.length; i++) {
          gaps.push(Math.abs(sorted[i].y - sorted[i - 1].y))
        }
        const allSame = gaps.every(g => roundClose(g, gaps[0], 0.05))
        if (allSame && gaps.length > 0) {
          pitch = Math.round(gaps[0] * 1000) / 1000
          pitchCol = sorted
          break
        }
      }
    }

    let leadSpan: number | null = null
    if (columns.length >= 2) {
      const leftX = columns[0][0]
      const rightX = columns[columns.length - 1][0]
      leadSpan = Math.abs(rightX - leftX)
    }

    let padSizeW: number | null = null
    let padSizeH: number | null = null
    if (signalPads.length > 0) {
      padSizeW = signalPads[0].w
      padSizeH = signalPads[0].h
    }

    const padExtent = {
      left: Math.min(-bw / 2, ...signalPads.map(p => p.x - p.w / 2)),
      right: Math.max(bw / 2, ...signalPads.map(p => p.x + p.w / 2)),
      top: Math.min(-bh / 2, ...signalPads.map(p => p.y - p.h / 2)),
      bottom: Math.max(bh / 2, ...signalPads.map(p => p.y + p.h / 2)),
    }

    const dimGap = 0.3
    const dimStandoff = 0.6

    const bodyWidthY = padExtent.bottom + dimStandoff
    annotations.push({
      type: 'horizontal',
      ax: -bw / 2, ay: bodyWidthY,
      bx: bw / 2, by: bodyWidthY,
      offset: dimGap,
      label: fmtMm(bw),
    })

    const bodyHeightX = padExtent.left - dimStandoff
    annotations.push({
      type: 'vertical',
      ax: bodyHeightX, ay: -bh / 2,
      bx: bodyHeightX, by: bh / 2,
      offset: dimGap,
      label: fmtMm(bh),
    })

    if (pitch !== null && pitchCol !== null && pitchCol.length >= 2) {
      const pitchX = padExtent.right + dimStandoff
      const p0 = pitchCol[0]
      const p1 = pitchCol[1]
      annotations.push({
        type: 'vertical',
        ax: pitchX, ay: p0.y,
        bx: pitchX, by: p1.y,
        offset: dimGap,
        label: fmtMm(pitch),
      })
    }

    if (leadSpan !== null && columns.length >= 2) {
      const leftX = columns[0][0]
      const rightX = columns[columns.length - 1][0]
      if (!roundClose(leadSpan, bw, 0.1)) {
        const leadSpanY = padExtent.top - dimStandoff
        annotations.push({
          type: 'horizontal',
          ax: leftX, ay: leadSpanY,
          bx: rightX, by: leadSpanY,
          offset: dimGap,
          label: fmtMm(leadSpan),
        })
      }
    }

    let padSizeLabel: { x: number; y: number; text: string } | null = null
    if (padSizeW !== null && padSizeH !== null && signalPads.length > 0) {
      padSizeLabel = {
        x: padExtent.left - dimStandoff,
        y: padExtent.bottom + dimStandoff + 0.55,
        text: `pad: ${fmtMm(padSizeW)} × ${fmtMm(padSizeH)}`,
      }
    }

    // --- viewBox ---
    let xMin = -bw / 2
    let xMax = bw / 2
    let yMin = -bh / 2
    let yMax = bh / 2

    for (const p of allP) {
      xMin = Math.min(xMin, p.x - p.w / 2)
      xMax = Math.max(xMax, p.x + p.w / 2)
      yMin = Math.min(yMin, p.y - p.h / 2)
      yMax = Math.max(yMax, p.y + p.h / 2)
    }

    const dimMargin = 2.2
    xMin -= dimMargin
    xMax += dimMargin
    yMin -= dimMargin
    yMax += dimMargin

    const extentW = xMax - xMin
    const extentH = yMax - yMin
    const canvasSize = 280
    const s = canvasSize / Math.max(extentW, extentH)

    return {
      viewBox: { xMin, yMin, width: extentW, height: extentH },
      scale: s,
      allPads: allP,
      dims: { annotations, padSizeLabel },
      layers: { fabPath, silkSegments, silkTrianglePath, courtyard },
    }
  }, [pads, body, thermalPad, courtyardMargin, pin1Chamfer])

  return (
    <div className="footprint-preview-widget">
      <div className="footprint-preview-header">
        <div className="footprint-preview-title">
          <span className="footprint-preview-badge">{attr.toUpperCase()}</span>
          <span className="footprint-preview-name">{name}</span>
        </div>
        <div className="footprint-preview-meta">
          {totalPads} pad{totalPads !== 1 ? 's' : ''}{thermalPad ? ' (+ EP)' : ''}
        </div>
        {description && <div className="footprint-preview-desc">{description}</div>}
      </div>

      <div className="footprint-diagram-container">
        <svg
          width={viewBox.width * scale}
          height={viewBox.height * scale}
          viewBox={`${viewBox.xMin} ${viewBox.yMin} ${viewBox.width} ${viewBox.height}`}
          className="footprint-diagram-svg"
        >
          <defs>
            <marker id="dim-arrow-start" markerWidth="6" markerHeight="4" refX="0" refY="2" orient="auto">
              <path d="M6,0 L0,2 L6,4" className="footprint-dim-arrowhead" />
            </marker>
            <marker id="dim-arrow-end" markerWidth="6" markerHeight="4" refX="6" refY="2" orient="auto">
              <path d="M0,0 L6,2 L0,4" className="footprint-dim-arrowhead" />
            </marker>
          </defs>

          {/* F.CrtYd — courtyard (back-most layer) */}
          <rect
            x={layers.courtyard.x}
            y={layers.courtyard.y}
            width={layers.courtyard.w}
            height={layers.courtyard.h}
            className="footprint-courtyard"
          />

          {/* F.Fab — body outline with pin-1 chamfer */}
          <path d={layers.fabPath} className="footprint-fab" />

          {/* F.SilkS — silkscreen outline, clipped around pads */}
          {layers.silkSegments.map((seg, i) => (
            <line key={`silk-${i}`}
              x1={seg.x1} y1={seg.y1} x2={seg.x2} y2={seg.y2}
              className="footprint-silk"
            />
          ))}
          <path d={layers.silkTrianglePath} className="footprint-silk-triangle" />

          {/* Pads */}
          {allPads.map((pad) => (
            <g key={`${pad.num}-${pad.isThermal ? 'ep' : 'sig'}`}>
              {pad.isCircle ? (
                <circle
                  cx={pad.x}
                  cy={pad.y}
                  r={pad.w / 2}
                  className={
                    pad.isThermal ? 'footprint-pad-thermal' :
                    pad.isPin1 ? 'footprint-pad-pin1' : 'footprint-pad'
                  }
                />
              ) : (
                <rect
                  x={pad.x - pad.w / 2}
                  y={pad.y - pad.h / 2}
                  width={pad.w}
                  height={pad.h}
                  rx={Math.min(pad.w, pad.h) * 0.15}
                  className={
                    pad.isThermal ? 'footprint-pad-thermal' :
                    pad.isPin1 ? 'footprint-pad-pin1' : 'footprint-pad'
                  }
                />
              )}
              <title>{`${pad.num}${pad.fn ? ` (${pad.fn})` : ''}`}</title>
              {allPads.length <= 20 && !pad.isThermal && (
                <text
                  x={pad.x}
                  y={pad.y}
                  className="footprint-pad-label"
                  textAnchor="middle"
                  dominantBaseline="central"
                  fontSize={Math.min(pad.w, pad.h) * 0.55}
                >
                  {pad.num}
                </text>
              )}
            </g>
          ))}

          {/* Pin 1 marker dot */}
          {allPads.find(p => p.isPin1) && (
            <circle
              cx={-body.width_mm / 2 - 0.3}
              cy={-body.height_mm / 2 - 0.3}
              r={0.15}
              className="footprint-pin1-marker"
            />
          )}

          {/* Dimension annotations */}
          {dims.annotations.map((dim, i) => (
            <DimensionLine key={i} dim={dim} />
          ))}

          {/* Pad size label */}
          {dims.padSizeLabel && (
            <text
              x={dims.padSizeLabel.x}
              y={dims.padSizeLabel.y}
              textAnchor="start"
              dominantBaseline="hanging"
              className="footprint-dim-text footprint-dim-pad-size"
            >
              {dims.padSizeLabel.text}
            </text>
          )}
        </svg>
      </div>

      <button
        className="symbol-preview-json-toggle"
        onClick={() => setShowJson(!showJson)}
      >
        {showJson ? 'Hide JSON' : 'View JSON'}
      </button>

      {showJson && (
        <pre className="symbol-preview-json">
          {JSON.stringify(fp, null, 2)}
        </pre>
      )}
    </div>
  )
}

function DimensionLine({ dim }: { dim: DimAnnotation }) {
  const { type, ax, ay, bx, by, offset, label } = dim
  const extLen = 0.3

  if (type === 'horizontal') {
    const dimY = ay
    const extDir = dimY > 0 ? -1 : 1

    return (
      <g>
        <line x1={ax} y1={dimY + extDir * extLen} x2={ax} y2={dimY - extDir * offset}
          className="footprint-dim-extension" />
        <line x1={bx} y1={dimY + extDir * extLen} x2={bx} y2={dimY - extDir * offset}
          className="footprint-dim-extension" />

        <line x1={ax} y1={dimY} x2={bx} y2={dimY}
          className="footprint-dim-line"
          markerStart="url(#dim-arrow-start)"
          markerEnd="url(#dim-arrow-end)"
        />

        <text
          x={(ax + bx) / 2}
          y={dimY - 0.15}
          textAnchor="middle"
          dominantBaseline="auto"
          className="footprint-dim-text"
        >
          {label}
        </text>
      </g>
    )
  }

  const dimX = ax
  const extDir = dimX < 0 ? 1 : -1

  return (
    <g>
      <line x1={dimX + extDir * extLen} y1={ay} x2={dimX - extDir * offset} y2={ay}
        className="footprint-dim-extension" />
      <line x1={dimX + extDir * extLen} y1={by} x2={dimX - extDir * offset} y2={by}
        className="footprint-dim-extension" />

      <line x1={dimX} y1={ay} x2={dimX} y2={by}
        className="footprint-dim-line"
        markerStart="url(#dim-arrow-start)"
        markerEnd="url(#dim-arrow-end)"
      />

      <text
        x={dimX}
        y={(ay + by) / 2}
        textAnchor="middle"
        dominantBaseline="auto"
        className="footprint-dim-text"
        transform={`rotate(-90, ${dimX}, ${(ay + by) / 2})`}
      >
        {label}
      </text>
    </g>
  )
}
