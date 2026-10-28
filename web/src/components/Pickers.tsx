// Inputs with suggestions: Home Assistant entities and MDI icons.
import { useMemo, useState } from 'preact/hooks';

import { ICONS } from '../generated/icons';
import type { HaEntity } from '../types';
import { Icon } from './Icon';

const ICON_NAMES = Object.keys(ICONS);

/**
 * @description Text input completed with the Home Assistant entities of the allowed domains.
 */
export function EntityPicker(props: {
  value: string;
  entities: HaEntity[] | null;
  domains?: string[];
  onChange: (value: string) => void;
}) {
  const listId = useMemo(() => `entities-${Math.random().toString(36).slice(2)}`, []);
  const options = (props.entities ?? []).filter((e) => !props.domains || props.domains.includes(e.id.split('.')[0]));
  return (
    <>
      <input
        list={listId}
        value={props.value}
        placeholder="light.salon"
        spellcheck={false}
        onInput={(e) => props.onChange(e.currentTarget.value.trim())}
      />
      <datalist id={listId}>
        {options.map((e) => (
          <option key={e.id} value={e.id}>
            {e.name}
          </option>
        ))}
      </datalist>
      {props.entities === null && <small class="hint">Liste des entités indisponible (Home Assistant non connecté).</small>}
    </>
  );
}

/**
 * @description Icon button opening a searchable grid of the icons compiled into the firmware.
 */
export function IconPicker(props: { value: string; placeholder?: string; onChange: (value: string) => void }) {
  const [open, setOpen] = useState(false);
  const [query, setQuery] = useState('');
  const shown = ICON_NAMES.filter((n) => n.includes(query.toLowerCase()));
  return (
    <div class="icon-picker">
      <button type="button" class="icon-current" onClick={() => setOpen(!open)}>
        <Icon name={props.value || props.placeholder || 'alert-circle-outline'} size={22} />
        <span>{props.value || `${props.placeholder ?? ''} (auto)`}</span>
      </button>
      {open && (
        <div class="icon-pop">
          <input autoFocus placeholder="Rechercher…" value={query} onInput={(e) => setQuery(e.currentTarget.value)} />
          <div class="icon-grid">
            {props.placeholder !== undefined && (
              <button
                type="button"
                title="Automatique"
                onClick={() => {
                  props.onChange('');
                  setOpen(false);
                }}
              >
                auto
              </button>
            )}
            {shown.map((name) => (
              <button
                type="button"
                key={name}
                title={name}
                class={name === props.value ? 'active' : ''}
                onClick={() => {
                  props.onChange(name);
                  setOpen(false);
                }}
              >
                <Icon name={name} size={22} />
              </button>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}
