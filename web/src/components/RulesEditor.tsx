// Editor of the display rules of a Home Assistant tile (props.rules).
import type { HaEntity, Rule, RuleCondition } from '../types';
import { Field } from './Field';
import { EntityPicker, IconPicker } from './Pickers';

type Operator = 'always' | 'is' | 'not' | 'above' | 'below' | 'between';

const OPERATORS: Record<Operator, string> = {
  always: 'toujours',
  is: 'est',
  not: 'n’est pas',
  above: 'supérieur à',
  below: 'inférieur à',
  between: 'entre',
};

/**
 * @description Operator shown for a condition (the first one found when a hand-written condition has several).
 */
function operatorOf(when: RuleCondition | undefined): Operator {
  if (!when) return 'always';
  if (when.is !== undefined) return 'is';
  if (when.not !== undefined) return 'not';
  if (when.above !== undefined && when.below !== undefined) return 'between';
  if (when.above !== undefined) return 'above';
  if (when.below !== undefined) return 'below';
  return 'always';
}

/**
 * @description Text of an "is"/"not" value: a list is shown comma-separated.
 */
function listText(value: string | string[] | undefined): string {
  return Array.isArray(value) ? value.join(', ') : (value ?? '');
}

/**
 * @description Parses a comma-separated list typed by the user ("on, home" → ["on", "home"]).
 */
function parseList(text: string): string | string[] {
  const items = text.split(',').map((s) => s.trim()).filter(Boolean);
  return items.length === 1 ? items[0] : items;
}

/**
 * @description Number typed by the user, or undefined when the field is empty or invalid.
 */
function parseNumber(text: string): number | undefined {
  const value = Number(text.replace(',', '.'));
  return text.trim() === '' || Number.isNaN(value) ? undefined : value;
}

/**
 * @description Removes the unset fields of a condition, and the condition itself when nothing is left.
 * An empty "is"/"not" value is kept so that the operator stays while the user types.
 */
function cleanCondition(when: RuleCondition): RuleCondition | undefined {
  const out = Object.fromEntries(Object.entries(when).filter(([, v]) => v !== undefined)) as RuleCondition;
  return Object.keys(out).length ? out : undefined;
}

/**
 * @description Fields of a condition: tested entity, attribute, operator and value(s). Shared by the
 * rules and by a widget's visible_if.
 */
export function ConditionFields(props: {
  when: RuleCondition | undefined;
  entities: HaEntity[] | null;
  onChange: (when: RuleCondition | undefined) => void;
  entityLabel?: string;
}) {
  const when = props.when ?? {};
  const op = operatorOf(props.when);
  const setWhen = (patch: RuleCondition) => props.onChange(cleanCondition({ ...when, ...patch }));
  const setOperator = (next: Operator) => {
    // Keep the target (entity, attribute) and start the value over
    const target = { entity: when.entity, attr: when.attr };
    if (next === 'always') props.onChange(cleanCondition(target));
    if (next === 'is' || next === 'not') props.onChange(cleanCondition({ ...target, [next]: '' }));
    if (next === 'above') props.onChange({ ...target, above: 0 });
    if (next === 'below') props.onChange({ ...target, below: 0 });
    if (next === 'between') props.onChange({ ...target, above: 0, below: 100 });
  };
  return (
    <>
      <Field label={props.entityLabel ?? 'Entité testée (vide = celle du widget)'}>
        <EntityPicker value={when.entity ?? ''} entities={props.entities} onChange={(v) => setWhen({ entity: v || undefined })} />
      </Field>
      <div class="row">
        <Field label="Attribut (vide = état)">
          <input
            value={when.attr ?? ''}
            placeholder="état"
            spellcheck={false}
            onInput={(e) => setWhen({ attr: e.currentTarget.value.trim() || undefined })}
          />
        </Field>
        <Field label="Condition">
          <select value={op} onChange={(e) => setOperator(e.currentTarget.value as Operator)}>
            {Object.entries(OPERATORS).map(([key, label]) => (
              <option key={key} value={key}>
                {label}
              </option>
            ))}
          </select>
        </Field>
      </div>
      {(op === 'is' || op === 'not') && (
        <Field label="Valeur(s), séparées par des virgules">
          <input
            value={listText(when[op])}
            placeholder="off, unavailable"
            spellcheck={false}
            onInput={(e) => setWhen({ [op]: parseList(e.currentTarget.value) })}
          />
        </Field>
      )}
      {op === 'above' && (
        <Field label="Strictement supérieur à">
          <input value={when.above ?? ''} inputMode="decimal" onInput={(e) => setWhen({ above: parseNumber(e.currentTarget.value) ?? 0 })} />
        </Field>
      )}
      {op === 'below' && (
        <Field label="Strictement inférieur à">
          <input value={when.below ?? ''} inputMode="decimal" onInput={(e) => setWhen({ below: parseNumber(e.currentTarget.value) ?? 0 })} />
        </Field>
      )}
      {op === 'between' && (
        <div class="row">
          <Field label="Plus de">
            <input value={when.above ?? ''} inputMode="decimal" onInput={(e) => setWhen({ above: parseNumber(e.currentTarget.value) ?? 0 })} />
          </Field>
          <Field label="Moins de">
            <input value={when.below ?? ''} inputMode="decimal" onInput={(e) => setWhen({ below: parseNumber(e.currentTarget.value) ?? 0 })} />
          </Field>
        </div>
      )}
    </>
  );
}

/**
 * @description Form of one rule: condition, then what it changes.
 */
function RuleRow(props: {
  rule: Rule;
  index: number;
  count: number;
  entities: HaEntity[] | null;
  onChange: (rule: Rule) => void;
  onMove: (delta: number) => void;
  onDelete: () => void;
}) {
  const { rule, index, count } = props;
  const set = (patch: Partial<Rule>) => {
    const next = { ...rule, ...patch };
    for (const key of Object.keys(patch) as (keyof Rule)[]) if (next[key] === undefined || next[key] === '') delete next[key];
    props.onChange(next);
  };

  return (
    <div class="rule">
      <div class="rule-head">
        <span>Règle {index + 1}</span>
        <button type="button" class="ghost" title="Monter" disabled={index === 0} onClick={() => props.onMove(-1)}>
          ↑
        </button>
        <button type="button" class="ghost" title="Descendre" disabled={index === count - 1} onClick={() => props.onMove(1)}>
          ↓
        </button>
        <button type="button" class="ghost" title="Supprimer" onClick={props.onDelete}>
          ✕
        </button>
      </div>

      <ConditionFields when={rule.when} entities={props.entities} onChange={(when) => props.onChange({ ...rule, when })} />

      <Field label="Texte affiché (vide = normal)">
        <input value={rule.text ?? ''} placeholder="{state} °C" onInput={(e) => set({ text: e.currentTarget.value })} />
      </Field>
      <div class="row">
        <Field label="Icône">
          <IconPicker value={rule.icon ?? ''} placeholder="" onChange={(v) => set({ icon: v || undefined })} />
        </Field>
        <Field label="Couleur">
          <div class="row rule-color">
            <input type="checkbox" checked={rule.color !== undefined} onChange={(e) => set({ color: e.currentTarget.checked ? '#F87171' : undefined })} />
            {rule.color !== undefined && (
              <input type="color" value={rule.color} onInput={(e) => set({ color: e.currentTarget.value.toUpperCase() })} />
            )}
          </div>
        </Field>
      </div>
      <Field label="Carte colorée (« allumée »)">
        <select
          value={rule.active === undefined ? 'auto' : rule.active ? 'on' : 'off'}
          onChange={(e) => {
            const v = e.currentTarget.value;
            set({ active: v === 'auto' ? undefined : v === 'on' });
          }}
        >
          <option value="auto">Selon l’état</option>
          <option value="on">Oui</option>
          <option value="off">Non</option>
        </select>
      </Field>
    </div>
  );
}

/**
 * @description List of rules of a widget. The first matching rule applies on the screen.
 */
export function RulesEditor(props: { rules: Rule[] | undefined; entities: HaEntity[] | null; onChange: (rules: Rule[] | undefined) => void }) {
  const rules = props.rules ?? [];
  const update = (next: Rule[]) => props.onChange(next.length ? next : undefined);
  const move = (index: number, delta: number) => {
    const next = [...rules];
    const [item] = next.splice(index, 1);
    next.splice(index + delta, 0, item);
    update(next);
  };

  return (
    <div class="rules">
      <h3>Règles d’affichage</h3>
      {rules.map((rule, i) => (
        <RuleRow
          key={i}
          rule={rule}
          index={i}
          count={rules.length}
          entities={props.entities}
          onChange={(r) => update(rules.map((old, j) => (j === i ? r : old)))}
          onMove={(delta) => move(i, delta)}
          onDelete={() => update(rules.filter((_, j) => j !== i))}
        />
      ))}
      <button type="button" disabled={rules.length >= 12} onClick={() => update([...rules, { when: { is: 'off' }, text: '' }])}>
        + Ajouter une règle
      </button>
      <small class="hint">
        La première règle vraie s’applique. Dans le texte : {'{state}'} (état), {'{name}'} (nom), {'{attr.xxx}'} (attribut). Exemples : état
        « off » → « Hors ligne » en rouge ; température supérieure à 25 → couleur orange.
      </small>
    </div>
  );
}
