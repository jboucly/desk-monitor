// Interactive 480x480 preview of a page: select, drag to move, drag the corner to resize, click an empty cell to add.
import { useRef, useState } from 'preact/hooks';

import { CATALOG, FLAT_BACKGROUND, PC_METRICS, WALLPAPERS, entityIcon, fits } from '../catalog';
import type { Page, Theme, Widget } from '../types';
import { GRID } from '../types';
import { Icon } from './Icon';

/**
 * @description Ongoing pointer gesture on a widget.
 */
interface Drag {
  id: string;
  mode: 'move' | 'resize';
  startX: number;
  startY: number;
  origin: Widget;
}

interface PreviewProps {
  page: Page;
  theme: Theme;
  selected: string | null;
  onSelect: (id: string | null) => void;
  onChange: (widget: Widget) => void;
  onAddAt: (x: number, y: number) => void;
}

/**
 * @description Simplified rendering of a widget, close enough to the device to recognize it.
 */
function WidgetFace({ widget, theme }: { widget: Widget; theme: Theme }) {
  const p = widget.props;
  const big = widget.w >= 2 && widget.h >= 2;
  switch (widget.type) {
    case 'clock':
      return (
        <div class="face face-clock">
          <span class={big ? 'clock-big' : 'clock'}>12:34</span>
          {p.show_date !== false && widget.h * widget.w >= 2 && <span class="muted">mercredi 28 octobre</span>}
        </div>
      );
    case 'weather':
      return p.mode === 'forecast' ? (
        <div class="face face-row">
          {['auj.', 'jeu.', 'ven.', 'sam.', 'dim.'].slice(0, Math.max(1, Math.min(p.days ?? 4, widget.w * 2))).map((d) => (
            <div class="forecast-day" key={d}>
              <span class="muted">{d}</span>
              <Icon name="weather-partly-cloudy" size={22} color="#ffe3a3" />
              <span>14°</span>
            </div>
          ))}
        </div>
      ) : (
        <div class="face face-weather">
          <Icon name="weather-partly-cloudy" size={big ? 56 : 30} color="#ffd166" />
          <span class={big ? 'value-big' : 'value'}>13°</span>
        </div>
      );
    case 'pc_metrics':
      return (
        <div class="face face-metrics">
          <span class="small ellipsis">
            <Icon name="desktop-tower" size={14} color={theme.accent} /> {p.name || 'Mon PC'}
          </span>
          {[['CPU', 34], ['RAM', 61], ['GPU', 12]].slice(0, widget.h >= 2 ? 3 : 1).map(([label, value]) => (
            <div class="metric-row" key={label}>
              <span class="muted small">{label}</span>
              <span class="metric-bar">
                <span style={{ width: `${value}%`, background: theme.accent }} />
              </span>
              <span class="small">{value} %</span>
            </div>
          ))}
        </div>
      );
    case 'pc_gauge':
    case 'ha_gauge':
      return (
        <div class="face face-gauge">
          <svg viewBox="0 0 100 100" class="gauge">
            <path d="M 22 78 A 40 40 0 1 1 78 78" class="gauge-track" />
            <path d="M 22 78 A 40 40 0 1 1 90 45" class="gauge-value" style={{ stroke: theme.accent }} />
          </svg>
          <span class="muted small ellipsis">
            {p.name || (widget.type === 'pc_gauge' ? PC_METRICS[p.metric ?? 'cpu']?.label : p.entity) || 'Jauge'}
          </span>
        </div>
      );
    case 'shortcut':
      return (
        <div class={`face ${widget.w >= 2 && widget.h === 1 ? 'face-row-start' : 'face-center'}`}>
          <span class="badge" style={{ background: `linear-gradient(${p.color || theme.accent}, ${p.color || theme.accent}99)` }}>
            <Icon name={p.icon || 'application'} size={22} color="#fff" />
          </span>
          <span class="small ellipsis">{p.name || 'Raccourci'}</span>
        </div>
      );
    case 'ha_sensor':
      return (
        <div class="face face-sensor">
          <span class="muted small ellipsis">
            <Icon name={p.icon || entityIcon(p.entity || 'sensor.x')} size={14} color={theme.accent} /> {p.name || p.entity || 'Capteur'}
          </span>
          <span class="value">
            21,5 <span class="muted small">{p.unit || ''}</span>
          </span>
        </div>
      );
    case 'ha_light':
    case 'ha_toggle':
      return (
        <div class="face face-entity">
          <span class="bubble">
            <Icon name={p.icon || entityIcon(p.entity || 'light.x')} size={big ? 28 : 20} />
          </span>
          <span class="ellipsis">{p.name || p.entity || CATALOG[widget.type].label}</span>
        </div>
      );
    default:
      return (
        <div class="face face-center muted">
          <Icon name={CATALOG[widget.type].icon} size={24} />
          <span class="small">Bientôt</span>
        </div>
      );
  }
}

/**
 * @description The page as it will look on the screen, at 1:1 scale on wide windows.
 */
export function Preview({ page, theme, selected, onSelect, onChange, onAddAt }: PreviewProps) {
  const gridRef = useRef<HTMLDivElement>(null);
  const [drag, setDrag] = useState<Drag | null>(null);
  const [ghost, setGhost] = useState<Widget | null>(null);

  // Size of one grid step in screen pixels (cell + gap), measured live because the preview scales
  const step = () => {
    const rect = gridRef.current!.getBoundingClientRect();
    return { x: rect.width / GRID, y: rect.height / GRID };
  };

  const onPointerDown = (e: PointerEvent, widget: Widget, mode: Drag['mode']) => {
    e.stopPropagation();
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    onSelect(widget.id);
    setDrag({ id: widget.id, mode, startX: e.clientX, startY: e.clientY, origin: widget });
  };

  const onPointerMove = (e: PointerEvent) => {
    if (!drag) return;
    const s = step();
    const dx = Math.round((e.clientX - drag.startX) / s.x);
    const dy = Math.round((e.clientY - drag.startY) / s.y);
    const o = drag.origin;
    const next =
      drag.mode === 'move'
        ? { ...o, x: o.x + dx, y: o.y + dy }
        : { ...o, w: Math.max(1, o.w + dx), h: Math.max(1, o.h + dy) };
    setGhost(fits(page, next.x, next.y, next.w, next.h, o.id) ? next : null);
  };

  const onPointerUp = () => {
    if (drag && ghost) onChange(ghost);
    setDrag(null);
    setGhost(null);
  };

  // Cells not covered by any widget get an "add" button
  const free: { x: number; y: number }[] = [];
  for (let y = 0; y < GRID; y++) for (let x = 0; x < GRID; x++) if (fits(page, x, y, 1, 1)) free.push({ x, y });

  return (
    <div
      class={`screen screen-${theme.style}`}
      style={{ background: theme.style === 'flat' ? FLAT_BACKGROUND : WALLPAPERS[theme.wallpaper]?.css(theme.accent, theme.accent2) }}
    >
      <div class="screen-header">
        <span>{page.title}</span>
        <span class="screen-status">
          <Icon name="laptop" size={16} color="#7d86a3" />
          <Icon name="home-assistant" size={16} color="#b4bcd6" />
          <Icon name="wifi-strength-4" size={16} color="#b4bcd6" />
          12:34
        </span>
      </div>
      <div class="grid" ref={gridRef} onPointerMove={onPointerMove} onPointerUp={onPointerUp} onClick={() => onSelect(null)}>
        {free.map(({ x, y }) => (
          <button
            key={`${x}-${y}`}
            class="cell-add"
            style={{ gridColumn: x + 1, gridRow: y + 1 }}
            title="Ajouter un widget"
            onClick={(e) => {
              e.stopPropagation();
              onAddAt(x, y);
            }}
          >
            <Icon name="plus" size={18} />
          </button>
        ))}
        {page.widgets.map((w) => {
          const shown = ghost && ghost.id === w.id ? ghost : w;
          const transparent = w.type === 'clock' && !w.props.card;
          return (
            <div
              key={w.id}
              class={`card ${transparent ? 'card-bare' : ''} ${selected === w.id ? 'selected' : ''} ${drag?.id === w.id ? 'dragging' : ''}`}
              style={{ gridColumn: `${shown.x + 1} / span ${shown.w}`, gridRow: `${shown.y + 1} / span ${shown.h}` }}
              onPointerDown={(e) => onPointerDown(e, w, 'move')}
              onClick={(e) => e.stopPropagation()}
            >
              <WidgetFace widget={w} theme={theme} />
              {selected === w.id && <span class="resize" onPointerDown={(e) => onPointerDown(e, w, 'resize')} />}
            </div>
          );
        })}
      </div>
    </div>
  );
}
