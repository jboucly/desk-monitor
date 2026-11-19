// Properties panel of the selected widget.
import { CATALOG, PC_METRICS, ENTITY_DOMAINS, entityIcon, fits } from '../catalog';
import { useState } from 'preact/hooks';

import { api } from '../api';
import type { Action, Gesture, HaEntity, ListItem, Page, Props, Widget, WidgetType } from '../types';
import { GRID } from '../types';
import { Field } from './Field';
import { EntityPicker, IconPicker } from './Pickers';
import { ConditionFields, RulesEditor } from './RulesEditor';

/**
 * @description Home Assistant tiles with an icon and display rules.
 */
const HA_TILES: string[] = ['ha_light', 'ha_toggle', 'ha_sensor', 'ha_climate', 'ha_cover', 'ha_slider', 'ha_select', 'ha_history', 'ha_alarm'];

interface InspectorProps {
  widget: Widget;
  page: Page;
  entities: HaEntity[] | null;
  onChange: (widget: Widget) => void;
  onDelete: () => void;
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
 * @description Labels of the action kinds, PC kinds last.
 */
const ACTION_LABELS: Record<Action['kind'], string> = {
  toggle: 'Allumer / éteindre',
  more_info: 'Ouvrir les réglages',
  none: 'Rien',
  ha: 'Service Home Assistant',
  launch: 'Lancer une application (PC)',
  url: 'Ouvrir une URL (PC)',
  keys: 'Raccourci clavier (PC)',
};

/**
 * @description Editor of an action (kind + its fields + confirmation). Tile gestures also offer toggle,
 * settings popup and nothing, and "default" (no action in the layout) when `defaultLabel` is given.
 */
function ActionEditor(props: {
  action: Action | undefined;
  entities: HaEntity[] | null;
  onChange: (a: Action | undefined) => void;
  tile?: boolean;
  label?: string;
  defaultLabel?: string;
}) {
  const { action, entities, onChange } = props;
  const setKind = (kind: Action['kind'] | 'default') => {
    const confirm = action?.confirm;
    if (kind === 'default') return onChange(undefined);
    if (kind === 'ha') onChange({ kind, domain: 'light', service: 'toggle', entity_id: '', confirm });
    if (kind === 'keys') onChange({ kind, keys: '', confirm });
    if (kind === 'launch') onChange({ kind, target: '', confirm });
    if (kind === 'url') onChange({ kind, url: '', confirm });
    if (kind === 'toggle' || kind === 'more_info') onChange({ kind, confirm });
    if (kind === 'none') onChange({ kind });
  };
  const kinds = (Object.keys(ACTION_LABELS) as Action['kind'][]).filter(
    (k) => props.tile || !['toggle', 'more_info', 'none'].includes(k),
  );
  return (
    <>
      <Field label={props.label ?? 'Action'}>
        <select value={action?.kind ?? 'default'} onChange={(e) => setKind(e.currentTarget.value as Action['kind'] | 'default')}>
          {props.defaultLabel && <option value="default">Par défaut : {props.defaultLabel}</option>}
          {kinds.map((k) => (
            <option key={k} value={k}>
              {ACTION_LABELS[k]}
            </option>
          ))}
        </select>
      </Field>
      {action && (action.kind === 'toggle' || action.kind === 'more_info') && (
        <Field label="Autre entité (vide = celle du widget)">
          <EntityPicker value={action.entity_id ?? ''} entities={entities} onChange={(v) => onChange({ ...action, entity_id: v || undefined })} />
        </Field>
      )}
      {action?.kind === 'launch' && (
        <Field label="Application">
          <input value={action.target} placeholder="Spotify" onInput={(e) => onChange({ ...action, target: e.currentTarget.value })} />
        </Field>
      )}
      {action?.kind === 'url' && (
        <Field label="URL">
          <input value={action.url} placeholder="https://…" onInput={(e) => onChange({ ...action, url: e.currentTarget.value })} />
        </Field>
      )}
      {action?.kind === 'keys' && (
        <Field label="Touches">
          <input
            value={action.keys}
            placeholder="cmd+shift+4"
            spellcheck={false}
            onInput={(e) => onChange({ ...action, keys: e.currentTarget.value })}
          />
          <small class="hint">
            Touches séparées par +, plusieurs combinaisons séparées par un espace. Modificateurs : cmd (⌘ sur Mac, Windows sur PC),
            ctrl, alt, shift. Touches spéciales : volumeup, volumedown, mute, playpause, next, prev, enter, esc, tab, space,
            backspace, delete, up, down, left, right, home, end, pageup, pagedown, f1 à f12.
          </small>
        </Field>
      )}
      {action?.kind === 'ha' && (
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
      {action && ['keys', 'launch', 'url'].includes(action.kind) && (
        <small class="hint">Exécuté par l’application Desk Monitor de l’ordinateur appairé, quand elle est connectée.</small>
      )}
      {action && action.kind !== 'none' && (
        <>
          <label class="check">
            <input
              type="checkbox"
              checked={!!action.confirm}
              onChange={(e) => onChange({ ...action, confirm: e.currentTarget.checked ? true : undefined })}
            />{' '}
            Demander confirmation
          </label>
          {action.confirm && (
            <Field label="Question (vide = « Nom : confirmer ? »)">
              <input
                value={typeof action.confirm === 'string' ? action.confirm : ''}
                placeholder="Ouvrir la porte du garage ?"
                onInput={(e) => onChange({ ...action, confirm: e.currentTarget.value || true })}
              />
            </Field>
          )}
        </>
      )}
    </>
  );
}

/**
 * @description What a tap and a long press do by default, per Home Assistant tile type.
 */
const GESTURE_DEFAULTS: Partial<Record<WidgetType, Record<Gesture, string>>> = {
  ha_toggle: { tap_action: 'allumer / éteindre', hold_action: 'réglages' },
  ha_light: { tap_action: 'allumer / éteindre', hold_action: 'réglages' },
  ha_sensor: { tap_action: 'rien', hold_action: 'rien' },
  ha_climate: { tap_action: 'réglages', hold_action: 'réglages' },
  ha_cover: { tap_action: 'réglages', hold_action: 'réglages' },
  ha_slider: { tap_action: 'réglages', hold_action: 'réglages' },
  ha_select: { tap_action: 'réglages', hold_action: 'réglages' },
  ha_alarm: { tap_action: 'clavier', hold_action: 'clavier' },
};

/**
 * @description Template field of a sensor, with a button that renders it once on Home Assistant.
 */
function TemplateField({ value, onChange }: { value: string; onChange: (v: string | undefined) => void }) {
  const [preview, setPreview] = useState<{ ok: boolean; text: string } | null>(null);
  const test = async () => {
    try {
      const { result } = await api.renderTemplate(value);
      setPreview({ ok: true, text: result });
    } catch (e) {
      setPreview({ ok: false, text: e instanceof Error ? e.message : String(e) });
    }
  };
  return (
    <Field label="Template Home Assistant (remplace l’entité)">
      <textarea
        rows={3}
        spellcheck={false}
        placeholder="{{ states('sensor.salon') | float | round(1) }}"
        value={value}
        onInput={(e) => {
          setPreview(null);
          onChange(e.currentTarget.value || undefined);
        }}
      />
      <div class="row template-test">
        <button type="button" disabled={!value.trim()} onClick={test}>
          Tester
        </button>
        {preview && <span class={preview.ok ? 'small' : 'small error'}>{preview.text || '(vide)'}</span>}
      </div>
      <small class="hint">Rendu par Home Assistant et mis à jour en direct (Jinja). 16 templates au plus par layout.</small>
    </Field>
  );
}

/**
 * @description Collapsible block whose open state belongs to the caller. Driving it from the data (e.g.
 * "open while the item has no entity") would fold it at the first letter typed.
 * Buttons in the summary must call preventDefault(), or the click also folds the block.
 */
function Collapsible(props: {
  open: boolean;
  onToggle: (open: boolean) => void;
  summary: preact.ComponentChildren;
  children: preact.ComponentChildren;
}) {
  return (
    <details class="rule" open={props.open} onToggle={(e) => props.onToggle((e.currentTarget as HTMLDetailsElement).open)}>
      <summary class="rule-head">{props.summary}</summary>
      {props.children}
    </details>
  );
}

/**
 * @description Items of an ha_list: each one an entity or a template, with its name, icon, rules and actions.
 */
function ListItemsEditor({ items, entities, onChange }: { items: ListItem[]; entities: HaEntity[] | null; onChange: (items: ListItem[]) => void }) {
  const update = (index: number, patch: Partial<ListItem>) => {
    const next = { ...items[index], ...patch };
    for (const key of Object.keys(patch) as (keyof ListItem)[]) if (next[key] === undefined || next[key] === '') delete next[key];
    onChange(items.map((item, i) => (i === index ? next : item)));
  };
  // Which blocks are unfolded: new or empty items start open; the flags move with their items
  const [opened, setOpened] = useState<boolean[]>(() => items.map((item) => !item.entity && !item.template));
  const move = (index: number, delta: number) => {
    const reorder = <T,>(list: T[]) => {
      const next = [...list];
      const [moved] = next.splice(index, 1);
      next.splice(index + delta, 0, moved);
      return next;
    };
    setOpened(reorder(opened));
    onChange(reorder(items));
  };
  const remove = (index: number) => {
    setOpened(opened.filter((_, j) => j !== index));
    onChange(items.filter((_, j) => j !== index));
  };
  const add = () => {
    setOpened([...opened, true]);
    onChange([...items, { entity: '' }]);
  };
  return (
    <div class="rules">
      <h3>Éléments</h3>
      {items.map((item, i) => (
        <Collapsible
          key={i}
          open={opened[i] ?? false}
          onToggle={(open) => open !== opened[i] && setOpened(opened.map((o, j) => (j === i ? open : o)))}
          summary={
            <>
              <span class="ellipsis">{item.name || item.entity || (item.template ? 'Template' : `Élément ${i + 1}`)}</span>
              <button type="button" class="ghost" title="Monter" disabled={i === 0} onClick={(e) => (e.preventDefault(), move(i, -1))}>
                ↑
              </button>
              <button
                type="button"
                class="ghost"
                title="Descendre"
                disabled={i === items.length - 1}
                onClick={(e) => (e.preventDefault(), move(i, 1))}
              >
                ↓
              </button>
              <button
                type="button"
                class="ghost"
                title="Supprimer"
                disabled={items.length === 1}
                onClick={(e) => (e.preventDefault(), remove(i))}
              >
                ✕
              </button>
            </>
          }
        >
          <Field label="Entité (ou un template ci-dessous)">
            <EntityPicker value={item.entity ?? ''} entities={entities} onChange={(v) => update(i, { entity: v })} />
          </Field>
          <TemplateField value={item.template ?? ''} onChange={(template) => update(i, { template })} />
          <div class="row">
            <Field label="Nom (vide = nom HA)">
              <input value={item.name ?? ''} onInput={(e) => update(i, { name: e.currentTarget.value })} />
            </Field>
            <Field label="Icône">
              <IconPicker value={item.icon ?? ''} placeholder={entityIcon(item.entity ?? '')} onChange={(v) => update(i, { icon: v || undefined })} />
            </Field>
          </div>
          <RulesEditor rules={item.rules} entities={entities} onChange={(rules) => update(i, { rules })} />
          {(['tap_action', 'hold_action'] as Gesture[]).map((gesture) => (
            <ActionEditor
              key={gesture}
              tile
              label={gesture === 'tap_action' ? 'Appui' : 'Appui long'}
              defaultLabel={gesture === 'tap_action' ? 'selon l’entité' : 'réglages'}
              action={item[gesture]}
              entities={entities}
              onChange={(action) => update(i, { [gesture]: action })}
            />
          ))}
        </Collapsible>
      ))}
      <button type="button" disabled={items.length >= 12} onClick={add}>
        + Ajouter une entité
      </button>
      <small class="hint">
        En pastilles, une entité est verte quand elle va bien : binary_sensor allumé (ou éteint pour fuite, fumée, problème…),
        interrupteur allumé, capteur disponible. Une règle « Carte colorée : Non » la marque en panne, « Oui » la marque OK.
      </small>
    </div>
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
      {(widget.type === 'pc_metrics' || widget.type === 'pc_gauge') && (
        <p class="hint">Données envoyées par l’app Desk Monitor de l’ordinateur appairé (Réglages → Ordinateur).</p>
      )}
      {widget.type.startsWith('claude_') && (
        <p class="hint">
          Sessions Claude Code de l’ordinateur appairé. Dans l’app Desk Monitor, active « Claude Code » : les demandes
          d’autorisation s’affichent alors sur l’écran, qui se réveille.
        </p>
      )}

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
          <Field label={widget.type === 'ha_sensor' ? 'Entité (ou un template ci-dessous)' : 'Entité'}>
            <EntityPicker value={p.entity ?? ''} entities={entities} domains={ENTITY_DOMAINS[widget.type]} onChange={(v) => set({ entity: v })} />
          </Field>
          {widget.type === 'ha_sensor' && <TemplateField value={p.template ?? ''} onChange={(template) => set({ template })} />}
          <Field label="Nom (vide = nom HA)">
            <input value={p.name ?? ''} onInput={(e) => set({ name: e.currentTarget.value || undefined })} />
          </Field>
        </>
      )}

      {HA_TILES.includes(widget.type) && (
        <Field label="Icône">
          <IconPicker value={p.icon ?? ''} placeholder={entityIcon(p.entity ?? '')} onChange={(v) => set({ icon: v || undefined })} />
        </Field>
      )}

      {widget.type === 'ha_sensor' && (
        <>
          <Field label="Affichage">
            <select value={p.format ?? 'auto'} onChange={(e) => set({ format: e.currentTarget.value === 'auto' ? undefined : e.currentTarget.value })}>
              <option value="auto">Automatique</option>
              <option value="number">Nombre</option>
              <option value="duration">Durée (ex. 12 j 4 h)</option>
            </select>
          </Field>
          <small class="hint">
            Automatique : les capteurs « date » de Home Assistant (comme le temps de fonctionnement de Proxmox) et les durées
            s’affichent en jours et heures.
          </small>
        </>
      )}

      {(widget.type === 'ha_gauge' || (widget.type === 'ha_sensor' && p.format !== 'duration')) && (
        <div class="row">
          <Field label="Unité (vide = HA)">
            <input value={p.unit ?? ''} onInput={(e) => set({ unit: e.currentTarget.value || undefined })} />
          </Field>
          <Field label="Décimales">
            <NumberInput value={p.decimals} onChange={(v) => set({ decimals: v })} />
          </Field>
        </div>
      )}

      {HA_TILES.includes(widget.type) && widget.type !== 'ha_history' && (
        <RulesEditor rules={p.rules} entities={entities} onChange={(rules) => set({ rules })} />
      )}

      {GESTURE_DEFAULTS[widget.type] && (
        <div class="rules">
          <h3>Actions</h3>
          {(['tap_action', 'hold_action'] as Gesture[]).map((gesture) => (
            <ActionEditor
              key={gesture}
              tile
              label={gesture === 'tap_action' ? 'Appui' : 'Appui long'}
              defaultLabel={GESTURE_DEFAULTS[widget.type]![gesture]}
              action={p[gesture] as Action | undefined}
              entities={entities}
              onChange={(action) => set({ [gesture]: action })}
            />
          ))}
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

      {widget.type === 'ha_climate' && (
        <small class="hint">
          Thermostat (climate) ou chauffe-eau (water_heater). Un appui ouvre les réglages : consigne, mode, préréglage, ventilation.
          À partir de 3 colonnes ou 2 lignes, les boutons − et + changent la consigne directement.
        </small>
      )}

      {widget.type === 'ha_alarm' && (
        <small class="hint">
          Alarme (alarm_control_panel) ou serrure (lock). Un appui ouvre les commandes, avec un clavier quand l’entité demande un
          code. Sur une tuile Interrupteur, une serrure sans code demande confirmation avant de se déverrouiller.
        </small>
      )}
      {widget.type === 'ha_sensor' && (
        <label class="check">
          <input type="checkbox" checked={!!p.show_age} onChange={(e) => set({ show_age: e.currentTarget.checked || undefined })} /> Afficher
          l’âge de la valeur (« il y a 5 min »)
        </label>
      )}

      {widget.type === 'ha_cover' && (
        <small class="hint">
          À partir de 2 colonnes, des boutons ouvrir/fermer (et stop dès 3 colonnes ou 2 lignes). Un appui ouvre la position et
          l’inclinaison.
        </small>
      )}
      {widget.type === 'ha_slider' && (
        <small class="hint">
          Valeur d’un input_number/number, luminosité d’une lumière, vitesse d’un ventilateur, volume d’un lecteur ou position d’un
          volet. Envoyée au relâchement du curseur.
        </small>
      )}
      {widget.type === 'ha_select' && (
        <small class="hint">Options d’un input_select/select, ou préréglages d’un ventilateur. Un appui ouvre la liste.</small>
      )}
      {(widget.type === 'ha_toggle' || widget.type === 'ha_light') && (
        <small class="hint">Appui long : réglages de l’entité (lumière, volet, ventilateur, thermostat, curseur, liste).</small>
      )}

      {widget.type === 'ha_list' && (
        <>
          <Field label="Titre">
            <input value={p.name ?? ''} placeholder="Statut" onInput={(e) => set({ name: e.currentTarget.value || undefined })} />
          </Field>
          <Field label="Affichage">
            <select value={p.mode ?? 'rows'} onChange={(e) => set({ mode: e.currentTarget.value })}>
              <option value="rows">Lignes (icône, nom, état)</option>
              <option value="badges">Pastilles vert / rouge (statut de services)</option>
            </select>
          </Field>
          <ListItemsEditor key={widget.id} items={(p.items as ListItem[]) ?? []} entities={entities} onChange={(items) => set({ items })} />
        </>
      )}

      {widget.type === 'ha_history' && (
        <>
          <div class="row">
            <Field label="Durée (heures)">
              <NumberInput value={p.hours ?? 24} onChange={(v) => set({ hours: Math.max(1, Math.min(168, v ?? 24)) })} />
            </Field>
            <Field label="Couleur">
              <input type="color" value={p.color ?? '#7C9CFF'} onInput={(e) => set({ color: e.currentTarget.value.toUpperCase() })} />
            </Field>
          </div>
          <div class="row">
            <Field label="Unité (vide = HA)">
              <input value={p.unit ?? ''} onInput={(e) => set({ unit: e.currentTarget.value || undefined })} />
            </Field>
            <Field label="Décimales">
              <NumberInput value={p.decimals} onChange={(v) => set({ decimals: v })} />
            </Field>
          </div>
          <div class="row">
            <Field label="Axe min (vide = auto)">
              <NumberInput value={p.min} onChange={(v) => set({ min: v })} />
            </Field>
            <Field label="Axe max (vide = auto)">
              <NumberInput value={p.max} onChange={(v) => set({ max: v })} />
            </Field>
          </div>
          <small class="hint">Historique de Home Assistant (1 à 168 h), réduit à 96 points et rechargé toutes les 5 minutes.</small>
        </>
      )}

      {widget.type === 'media' && (
        <>
          <Field label="Lecteur Home Assistant (facultatif)">
            <EntityPicker value={p.entity ?? ''} entities={entities} domains={['media_player']} onChange={(v) => set({ entity: v || undefined })} />
          </Field>
          <small class="hint">
            Affiche ce que joue l’ordinateur appairé (toutes les apps : Musique, Spotify, Chrome…). Quand il ne joue rien ou qu’il est
            éteint, le widget montre ce lecteur Home Assistant.
          </small>
        </>
      )}

      {widget.type === 'pc_metrics' && (
        <Field label="Titre (vide = nom du PC)">
          <input value={p.name ?? ''} onInput={(e) => set({ name: e.currentTarget.value || undefined })} />
        </Field>
      )}

      {widget.type.startsWith('claude_') && (
        <Field label="Titre">
          <input
            value={p.name ?? ''}
            placeholder={{ claude_sessions: 'Claude Code', claude_usage: 'Tokens' }[widget.type as string] ?? 'Limites'}
            onInput={(e) => set({ name: e.currentTarget.value || undefined })}
          />
        </Field>
      )}

      {widget.type === 'pc_gauge' && (
        <>
          <Field label="Mesure">
            <select value={p.metric ?? 'cpu'} onChange={(e) => set({ metric: e.currentTarget.value })}>
              {Object.entries(PC_METRICS).map(([key, m]) => (
                <option key={key} value={key}>
                  {m.label} ({m.unit})
                </option>
              ))}
            </select>
          </Field>
          <Field label="Nom (vide = nom de la mesure)">
            <input value={p.name ?? ''} onInput={(e) => set({ name: e.currentTarget.value || undefined })} />
          </Field>
          <div class="row">
            <Field label="Seuil orange">
              <NumberInput value={p.warn} onChange={(v) => set({ warn: v })} />
            </Field>
            <Field label="Seuil rouge">
              <NumberInput value={p.crit} onChange={(v) => set({ crit: v })} />
            </Field>
          </div>
          <small class="hint">Seuils vides : valeurs par défaut de la mesure.</small>
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
          <ActionEditor action={p.action as Action} entities={entities} onChange={(action) => action && set({ action })} />
        </>
      )}

      <div class="rules">
        <h3>Visibilité</h3>
        <label class="check">
          <input
            type="checkbox"
            checked={!!p.visible_if}
            onChange={(e) => set({ visible_if: e.currentTarget.checked ? { entity: p.entity || undefined, is: 'on' } : undefined })}
          />{' '}
          Visible seulement si…
        </label>
        {p.visible_if && (
          <>
            <ConditionFields
              when={p.visible_if}
              entities={entities}
              entityLabel={p.entity ? 'Entité testée (vide = celle du widget)' : 'Entité testée'}
              onChange={(visible_if) => set({ visible_if: visible_if ?? {} })}
            />
            <small class="hint">Sinon la case reste vide sur l’écran (aussi tant que Home Assistant n’a pas répondu).</small>
          </>
        )}
      </div>

      <button class="danger" onClick={onDelete}>
        Supprimer le widget
      </button>
    </div>
  );
}
