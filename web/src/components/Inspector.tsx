// Properties panel of the selected widget.
import { CATALOG, ENTITY_DOMAINS, entityIcon, fits } from '../catalog';
import type { Action, HaEntity, Page, Props, Widget } from '../types';
import { GRID } from '../types';
import { EntityPicker, IconPicker } from './Pickers';

interface InspectorProps {
  widget: Widget;
  page: Page;
  entities: HaEntity[] | null;
  onChange: (widget: Widget) => void;
  onDelete: () => void;
}

/**
 * @description Labeled form row.
 */
function Field({ label, children }: { label: string; children: preact.ComponentChildren }) {
  return (
    <label class="field">
      <span>{label}</span>
      {children}
    </label>
  );
}

/**
 * @description Number input that keeps empty values out of the props.
 */
function NumberInput(props: { value: number | undefined; step?: number; onChange: (v: number | undefined) => void }) {
  return (
    <input
      type="number"
      step={props.step ?? 1}
      value={props.value ?? ''}
      onInput={(e) => {
        const raw = e.currentTarget.value;
        props.onChange(raw === '' ? undefined : Number(raw));
      }}
    />
  );
}

/**
 * @description Editor of a shortcut action (kind + its fields).
 */
function ActionEditor({ action, entities, onChange }: { action: Action; entities: HaEntity[] | null; onChange: (a: Action) => void }) {
  const setKind = (kind: Action['kind']) => {
    if (kind === 'ha') onChange({ kind, domain: 'light', service: 'toggle', entity_id: '' });
    if (kind === 'keys') onChange({ kind, keys: '' });
    if (kind === 'launch') onChange({ kind, target: '' });
    if (kind === 'url') onChange({ kind, url: '' });
  };
  return (
    <>
      <Field label="Action">
        <select value={action.kind} onChange={(e) => setKind(e.currentTarget.value as Action['kind'])}>
          <option value="launch">Lancer une application (PC)</option>
          <option value="url">Ouvrir une URL (PC)</option>
          <option value="keys">Raccourci clavier (PC)</option>
          <option value="ha">Service Home Assistant</option>
        </select>
      </Field>
      {action.kind === 'launch' && (
        <Field label="Application">
          <input value={action.target} placeholder="Spotify" onInput={(e) => onChange({ ...action, target: e.currentTarget.value })} />
        </Field>
      )}
      {action.kind === 'url' && (
        <Field label="URL">
          <input value={action.url} placeholder="https://…" onInput={(e) => onChange({ ...action, url: e.currentTarget.value })} />
        </Field>
      )}
      {action.kind === 'keys' && (
        <Field label="Touches">
          <input
            value={action.keys}
            placeholder="cmd+shift+4"
            spellcheck={false}
            onInput={(e) => onChange({ ...action, keys: e.currentTarget.value })}
          />
          <small class="hint">Touches séparées par +, plusieurs combinaisons séparées par un espace. cmd = ⌘ sur Mac, Windows sur PC.</small>
        </Field>
      )}
      {action.kind === 'ha' && (
        <>
          <div class="row">
            <Field label="Domaine">
              <input value={action.domain} onInput={(e) => onChange({ ...action, domain: e.currentTarget.value.trim() })} />
            </Field>
            <Field label="Service">
              <input value={action.service} onInput={(e) => onChange({ ...action, service: e.currentTarget.value.trim() })} />
            </Field>
          </div>
          <Field label="Entité (optionnelle)">
            <EntityPicker value={action.entity_id ?? ''} entities={entities} onChange={(v) => onChange({ ...action, entity_id: v })} />
          </Field>
          <Field label="Données (JSON)">
            <textarea
              rows={3}
              spellcheck={false}
              placeholder='{"brightness_pct": 50}'
              value={action.data ? JSON.stringify(action.data) : ''}
              onChange={(e) => {
                const raw = e.currentTarget.value.trim();
                try {
                  onChange({ ...action, data: raw ? JSON.parse(raw) : undefined });
                } catch {
                  // Invalid JSON: keep the previous data until it parses
                }
              }}
            />
          </Field>
        </>
      )}
      {action.kind !== 'ha' && <small class="hint">Exécuté par l’application compagnon quand le PC est connecté en Bluetooth.</small>}
    </>
  );
}

/**
 * @description Form of the selected widget: size, then the props of its type.
 */
export function Inspector({ widget, page, entities, onChange, onDelete }: InspectorProps) {
  const info = CATALOG[widget.type];
  const p = widget.props;
  const set = (patch: Props) => onChange({ ...widget, props: { ...p, ...patch } });
  const resize = (w: number, h: number) => fits(page, widget.x, widget.y, w, h, widget.id) && onChange({ ...widget, w, h });
  const sizes = [1, 2, 3, 4];

  return (
    <div class="inspector">
      <h3>{info.label}</h3>
      {!info.available && <p class="hint">Ce widget arrive avec l’application compagnon (phase 2/3) : il s’affiche « Bientôt » sur l’écran.</p>}

      <div class="row">
        <Field label="Largeur">
          <select value={widget.w} onChange={(e) => resize(Number(e.currentTarget.value), widget.h)}>
            {sizes.filter((s) => widget.x + s <= GRID).map((s) => (
              <option key={s} value={s}>
                {s}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Hauteur">
          <select value={widget.h} onChange={(e) => resize(widget.w, Number(e.currentTarget.value))}>
            {sizes.filter((s) => widget.y + s <= GRID).map((s) => (
              <option key={s} value={s}>
                {s}
              </option>
            ))}
          </select>
        </Field>
      </div>

      {ENTITY_DOMAINS[widget.type] && (
        <>
          <Field label="Entité">
            <EntityPicker value={p.entity ?? ''} entities={entities} domains={ENTITY_DOMAINS[widget.type]} onChange={(v) => set({ entity: v })} />
          </Field>
          <Field label="Nom (vide = nom HA)">
            <input value={p.name ?? ''} onInput={(e) => set({ name: e.currentTarget.value || undefined })} />
          </Field>
        </>
      )}

      {(widget.type === 'ha_light' || widget.type === 'ha_toggle' || widget.type === 'ha_sensor') && (
        <Field label="Icône">
          <IconPicker value={p.icon ?? ''} placeholder={entityIcon(p.entity ?? '')} onChange={(v) => set({ icon: v || undefined })} />
        </Field>
      )}

      {(widget.type === 'ha_sensor' || widget.type === 'ha_gauge') && (
        <div class="row">
          <Field label="Unité (vide = HA)">
            <input value={p.unit ?? ''} onInput={(e) => set({ unit: e.currentTarget.value || undefined })} />
          </Field>
          <Field label="Décimales">
            <NumberInput value={p.decimals} onChange={(v) => set({ decimals: v })} />
          </Field>
        </div>
      )}

      {widget.type === 'ha_gauge' && (
        <>
          <div class="row">
            <Field label="Minimum">
              <NumberInput value={p.min} onChange={(v) => set({ min: v })} />
            </Field>
            <Field label="Maximum">
              <NumberInput value={p.max} onChange={(v) => set({ max: v })} />
            </Field>
          </div>
          <div class="row">
            <Field label="Seuil orange">
              <NumberInput value={p.warn} onChange={(v) => set({ warn: v })} />
            </Field>
            <Field label="Seuil rouge">
              <NumberInput value={p.crit} onChange={(v) => set({ crit: v })} />
            </Field>
          </div>
        </>
      )}

      {widget.type === 'clock' && (
        <>
          <label class="check">
            <input type="checkbox" checked={p.show_date !== false} onChange={(e) => set({ show_date: e.currentTarget.checked })} /> Afficher la date
          </label>
          <label class="check">
            <input type="checkbox" checked={!!p.show_seconds} onChange={(e) => set({ show_seconds: e.currentTarget.checked })} /> Afficher les secondes
          </label>
          <label class="check">
            <input type="checkbox" checked={!!p.card} onChange={(e) => set({ card: e.currentTarget.checked })} /> Fond en verre
          </label>
        </>
      )}

      {widget.type === 'weather' && (
        <>
          <Field label="Affichage">
            <select value={p.mode ?? 'current'} onChange={(e) => set({ mode: e.currentTarget.value })}>
              <option value="current">Conditions actuelles</option>
              <option value="forecast">Prévisions</option>
            </select>
          </Field>
          {p.mode === 'forecast' && (
            <Field label="Nombre de jours">
              <NumberInput value={p.days ?? 4} onChange={(v) => set({ days: Math.max(1, Math.min(7, v ?? 4)) })} />
            </Field>
          )}
          <small class="hint">Le lieu se règle dans Réglages → Météo.</small>
        </>
      )}

      {widget.type === 'shortcut' && (
        <>
          <Field label="Nom">
            <input value={p.name ?? ''} onInput={(e) => set({ name: e.currentTarget.value })} />
          </Field>
          <div class="row">
            <Field label="Icône">
              <IconPicker value={p.icon ?? ''} onChange={(v) => set({ icon: v })} />
            </Field>
            <Field label="Couleur">
              <input type="color" value={p.color ?? '#7C9CFF'} onInput={(e) => set({ color: e.currentTarget.value.toUpperCase() })} />
            </Field>
          </div>
          <ActionEditor action={p.action as Action} entities={entities} onChange={(action) => set({ action })} />
        </>
      )}

      <button class="danger" onClick={onDelete}>
        Supprimer le widget
      </button>
    </div>
  );
}
