import { useState, useMemo } from 'react'
import type { BomPreview, BomItem } from '../vite-env.d'

interface BomPreviewWidgetProps {
  bomPreview: BomPreview
}

type SortKey = 'reference' | 'quantity' | 'value' | 'mpn' | 'manufacturer' | 'unitPrice'
type SortDir = 'asc' | 'desc'

function formatPrice(price: number | null | undefined): string {
  if (price === null || price === undefined) return '—'
  return `$${Number(price).toFixed(4)}`
}

function lineTotal(item: BomItem): number | null {
  if (item.unit_price === null || item.unit_price === undefined) return null
  return Number(item.unit_price) * item.quantity
}

export default function BomPreviewWidget({ bomPreview }: BomPreviewWidgetProps) {
  const { bomItems, projectName, totalCost } = bomPreview
  const [sortKey, setSortKey] = useState<SortKey>('reference')
  const [sortDir, setSortDir] = useState<SortDir>('asc')
  const [expanded, setExpanded] = useState(true)

  const totalQty = useMemo(() => bomItems.reduce((s, i) => s + i.quantity, 0), [bomItems])

  const sorted = useMemo(() => {
    const copy = [...bomItems]
    copy.sort((a, b) => {
      let cmp = 0
      switch (sortKey) {
        case 'reference': cmp = a.reference.localeCompare(b.reference); break
        case 'quantity': cmp = a.quantity - b.quantity; break
        case 'value': cmp = a.value.localeCompare(b.value); break
        case 'mpn': cmp = a.mpn.localeCompare(b.mpn); break
        case 'manufacturer': cmp = a.manufacturer.localeCompare(b.manufacturer); break
        case 'unitPrice': cmp = (a.unit_price ?? 0) - (b.unit_price ?? 0); break
      }
      return sortDir === 'asc' ? cmp : -cmp
    })
    return copy
  }, [bomItems, sortKey, sortDir])

  const handleSort = (key: SortKey) => {
    if (sortKey === key) {
      setSortDir(d => d === 'asc' ? 'desc' : 'asc')
    } else {
      setSortKey(key)
      setSortDir('asc')
    }
  }

  const sortArrow = (key: SortKey) => {
    if (sortKey !== key) return null
    return <span className="bom-sort-arrow">{sortDir === 'asc' ? '▲' : '▼'}</span>
  }

  return (
    <div className="bom-preview-widget">
      <button className="bom-preview-header" onClick={() => setExpanded(e => !e)}>
        <div className="bom-preview-header-left">
          <svg className="bom-preview-icon" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z" />
            <polyline points="14 2 14 8 20 8" />
            <line x1="16" y1="13" x2="8" y2="13" />
            <line x1="16" y1="17" x2="8" y2="17" />
            <polyline points="10 9 9 9 8 9" />
          </svg>
          <span className="bom-preview-title">Bill of Materials</span>
          <span className="bom-preview-badge">{projectName}</span>
        </div>
        <div className="bom-preview-header-right">
          <span className="bom-preview-stat">{bomItems.length} line items</span>
          <span className="bom-preview-stat">{totalQty} parts</span>
          {totalCost !== null && (
            <span className="bom-preview-stat bom-preview-cost">${totalCost.toFixed(2)}</span>
          )}
          <svg
            className={`bom-preview-chevron ${expanded ? 'open' : ''}`}
            viewBox="0 0 24 24" width="14" height="14"
            fill="none" stroke="currentColor" strokeWidth="2"
          >
            <polyline points="6 9 12 15 18 9" />
          </svg>
        </div>
      </button>

      {expanded && (
        <div className="bom-preview-body">
          <div className="bom-table-wrapper">
            <table className="bom-table">
              <thead>
                <tr>
                  <th className="bom-th bom-th-sortable" onClick={() => handleSort('quantity')}>Qty {sortArrow('quantity')}</th>
                  <th className="bom-th bom-th-sortable" onClick={() => handleSort('reference')}>Reference {sortArrow('reference')}</th>
                  <th className="bom-th bom-th-sortable" onClick={() => handleSort('value')}>Value {sortArrow('value')}</th>
                  <th className="bom-th">Footprint</th>
                  <th className="bom-th bom-th-sortable" onClick={() => handleSort('mpn')}>MPN {sortArrow('mpn')}</th>
                  <th className="bom-th bom-th-sortable" onClick={() => handleSort('manufacturer')}>Manufacturer {sortArrow('manufacturer')}</th>
                  <th className="bom-th bom-th-sortable bom-th-price" onClick={() => handleSort('unitPrice')}>Unit Price {sortArrow('unitPrice')}</th>
                  <th className="bom-th bom-th-price">Line Total</th>
                  <th className="bom-th">Suppliers</th>
                  <th className="bom-th">Datasheet</th>
                </tr>
              </thead>
              <tbody>
                {sorted.map((item, i) => (
                  <tr key={i} className="bom-row">
                    <td className="bom-td bom-td-qty">{item.quantity}</td>
                    <td className="bom-td bom-td-ref">{item.reference}</td>
                    <td className="bom-td">{item.value}</td>
                    <td className="bom-td bom-td-fp">{item.footprint}</td>
                    <td className="bom-td bom-td-mpn">{item.mpn}</td>
                    <td className="bom-td">{item.manufacturer}</td>
                    <td className="bom-td bom-td-price">{formatPrice(item.unit_price)}</td>
                    <td className="bom-td bom-td-price">{formatPrice(lineTotal(item))}</td>
                    <td className="bom-td bom-td-suppliers">
                      {item.suppliers && item.suppliers.length > 0 ? (
                        <div className="bom-supplier-pills">
                          {item.suppliers.map((s, j) => (
                            s.url ? (
                              <a
                                key={j}
                                className="bom-supplier-pill"
                                href={s.url}
                                target="_blank"
                                rel="noopener noreferrer"
                                title={s.price !== null ? `$${s.price} — ${s.stock ?? '?'} in stock` : s.name}
                              >
                                {s.name}
                              </a>
                            ) : (
                              <span key={j} className="bom-supplier-pill bom-supplier-pill-disabled">{s.name}</span>
                            )
                          ))}
                        </div>
                      ) : '—'}
                    </td>
                    <td className="bom-td bom-td-ds">
                      {item.datasheet_url ? (
                        <a className="bom-ds-link" href={item.datasheet_url} target="_blank" rel="noopener noreferrer" title="Open datasheet">
                          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                            <path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6" />
                            <polyline points="15 3 21 3 21 9" />
                            <line x1="10" y1="14" x2="21" y2="3" />
                          </svg>
                        </a>
                      ) : '—'}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          <div className="bom-summary">
            <span className="bom-summary-item">{bomItems.length} unique parts</span>
            <span className="bom-summary-sep">·</span>
            <span className="bom-summary-item">{totalQty} total components</span>
            {totalCost !== null && (
              <>
                <span className="bom-summary-sep">·</span>
                <span className="bom-summary-item bom-summary-cost">Est. ${totalCost.toFixed(2)}</span>
              </>
            )}
          </div>
        </div>
      )}
    </div>
  )
}
