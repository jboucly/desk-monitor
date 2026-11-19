// Inputs with suggestions: Home Assistant entities and MDI icons.
import { useMemo, useState } from 'preact/hooks';

import { ICONS } from '../generated/icons';
import type { HaEntity } from '../types';
import { Icon } from './Icon';

const ICON_NAMES = Object.keys(ICONS);

/**
 * @description Most suggestions shown at once: typing narrows the list.
 */
const MAX_SUGGESTIONS = 60;

/**
 * @description Text input completed with the Home Assistant entities of the allowed domains. The
 * suggestions are our own list under the field: the native <datalist> popup opens far away from the
 * input in the macOS webview of the companion's editor window.
 */
export function EntityPicker(props: {
  value: string;
  entities: HaEntity[] | null;
  domains?: string[];
  onChange: (value: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const [active, setActive] = useState(0);
  const query = props.value.toLowerCase();
  const matches = useMemo(() => {
    const allowed = (props.entities ?? []).filter((e) => !props.domains || props.domains.includes(e.id.split('.')[0]));
    return allowed.filter((e) => !query || e.id.toLowerCase().includes(query) || e.name.toLowerCase().includes(query));
  }, [props.entities, props.domains, query]);
  const shown = matches.slice(0, MAX_SUGGESTIONS);

  const pick = (id: string) => {
    props.onChange(id);
    setOpen(false);
  };
  const onKeyDown = (e: KeyboardEvent) => {
    if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      setOpen(true);
      const delta = e.key === 'ArrowDown' ? 1 : -1;
      setActive((i) => Math.max(0, Math.min(shown.length - 1, i + delta)));
    } else if (e.key === 'Enter' && open && shown[active]) {
      e.preventDefault();
      pick(shown[active].id);
    } else if (e.key === 'Escape') {
      setOpen(false);
    }
  };

  return (
    <div class="entity-picker">
      <input
        value={props.value}
        placeholder="light.salon"
        spellcheck={false}
        autocomplete="off"
        onFocus={() => setOpen(true)}
        onBlur={() => setOpen(false)}
        onKeyDown={onKeyDown}
        onInput={(e) => {
          props.onChange(e.currentTarget.value.trim());
          setActive(0);
          setOpen(true);
        }}
      />
      {open && shown.length > 0 && !(shown.length === 1 && shown[0].id === props.value) && (
        <ul class="entity-pop" role="listbox">
          {shown.map((e, i) => (
            <li
              key={e.id}
              role="option"
              aria-selected={i === active}
              class={i === active ? 'active' : ''}
              // mousedown: pick before the input's blur closes the list
              onMouseDown={(event) => {
                event.preventDefault();
                pick(e.id);
              }}
              onMouseEnter={() => setActive(i)}
            >
              <span class="ellipsis">{e.id}</span>
              {e.name && <small class="ellipsis">{e.name}</small>}
            </li>
          ))}
          {matches.length > shown.length && <li class="entity-more">… {matches.length - shown.length} autres, précise la recherche</li>}
        </ul>
      )}
      {props.entities === null && <small class="hint">Liste des entités indisponible (Home Assistant non connecté).</small>}
    </div>
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
