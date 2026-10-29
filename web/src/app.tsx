// Editor shell: tabs (Pages / Réglages / Système), layout loading and saving, toasts.
import { useEffect, useState } from 'preact/hooks';

import { ApiError, api, setPin } from './api';
import { CATALOG, EMPTY_LAYOUT, createWidget, fits } from './catalog';
import { askText } from './components/Dialog';
import { Icon } from './components/Icon';
import { Inspector } from './components/Inspector';
import { Preview } from './components/Preview';
import { SettingsView } from './components/SettingsView';
import { Sidebar } from './components/Sidebar';
import { SystemView } from './components/SystemView';
import type { HaEntity, Layout, Widget, WidgetType } from './types';

type Tab = 'pages' | 'settings' | 'system';

/**
 * @description Root component of the editor.
 */
export function App() {
  const [tab, setTab] = useState<Tab>('pages');
  const [layout, setLayout] = useState<Layout>(EMPTY_LAYOUT);
  const [saved, setSaved] = useState('');
  const [pageIndex, setPageIndex] = useState(0);
  const [selected, setSelected] = useState<string | null>(null);
  const [entities, setEntities] = useState<HaEntity[] | null>(null);
  const [adding, setAdding] = useState<{ x: number; y: number } | null>(null);
  const [toast, setToast] = useState<{ text: string; error: boolean } | null>(null);
  const [loaded, setLoaded] = useState(false);

  const dirty = loaded && JSON.stringify(layout) !== saved;
  const page = layout.pages[Math.min(pageIndex, layout.pages.length - 1)];
  const widget = page.widgets.find((w) => w.id === selected) ?? null;

  const notify = (text: string, error = false) => {
    setToast({ text, error });
    setTimeout(() => setToast(null), 3500);
  };

  // A 401 means a PIN is set: ask for it, then let the user retry
  const fail = async (e: unknown) => {
    if (e instanceof ApiError && e.status === 401) {
      const pin = await askText('Code PIN de l’écran :', { secret: true });
      if (pin !== null) {
        setPin(pin);
        return notify('PIN mémorisé pour cet onglet, réessaie.');
      }
    }
    notify(e instanceof Error ? e.message : String(e), true);
  };

  useEffect(() => {
    api.getLayout().then(
      (l) => {
        // "id" and "props" are optional in layout.json, the editor relies on both
        l.pages.forEach((p, pi) =>
          p.widgets.forEach((w, i) => {
            w.id ||= `w${pi}-${i}`;
            w.props ??= {};
          }),
        );
        l.theme = { ...EMPTY_LAYOUT.theme, ...l.theme };
        setLayout(l);
        setSaved(JSON.stringify(l));
        setLoaded(true);
      },
      fail,
    );
    api.getEntities().then(setEntities, () => setEntities(null));
  }, []);

  useEffect(() => {
    const warn = (e: BeforeUnloadEvent) => dirty && e.preventDefault();
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [dirty]);

  const updatePage = (patch: (widgets: Widget[]) => Widget[]) =>
    setLayout({ ...layout, pages: layout.pages.map((p, i) => (i === pageIndex ? { ...p, widgets: patch(p.widgets) } : p)) });

  const updateWidget = (w: Widget) => updatePage((ws) => ws.map((o) => (o.id === w.id ? w : o)));
  const deleteWidget = (id: string) => {
    updatePage((ws) => ws.filter((o) => o.id !== id));
    setSelected(null);
  };

  const addWidget = (type: WidgetType) => {
    if (!adding) return;
    const info = CATALOG[type];
    let w = createWidget(page, type);
    // Prefer the clicked cell, with the default size when it fits there
    for (const [ww, hh] of [[info.w, info.h], [Math.min(info.w, 2), Math.min(info.h, 2)], [1, 1]]) {
      if (fits(page, adding.x, adding.y, ww, hh)) {
        w = { id: w?.id ?? `w${Date.now()}`, type, x: adding.x, y: adding.y, w: ww, h: hh, props: info.defaults() };
        break;
      }
    }
    setAdding(null);
    if (!w) return notify('La page est pleine', true);
    updatePage((ws) => [...ws, w!]);
    setSelected(w.id);
  };

  const save = async () => {
    try {
      await api.putLayout(layout);
      setSaved(JSON.stringify(layout));
      notify('Layout envoyé à l’écran');
    } catch (e) {
      fail(e);
    }
  };

  return (
    <div class="app">
      <header class="topbar">
        <div class="brand">
          <Icon name="monitor" size={22} color="var(--accent)" />
          Desk Monitor
        </div>
        <nav class="tabs">
          {(
            [
              ['pages', 'Pages'],
              ['settings', 'Réglages'],
              ['system', 'Système'],
            ] as [Tab, string][]
          ).map(([key, label]) => (
            <button key={key} class={tab === key ? 'active' : ''} onClick={() => setTab(key)}>
              {label}
            </button>
          ))}
        </nav>
        {tab === 'pages' && (
          <div class="save">
            {dirty && <span class="muted">Modifications non envoyées</span>}
            <button class="ghost" disabled={!dirty} onClick={() => setLayout(JSON.parse(saved))}>
              Annuler
            </button>
            <button class="primary" disabled={!dirty} onClick={save}>
              Envoyer à l’écran
            </button>
          </div>
        )}
      </header>

      {tab === 'pages' && (
        <main class="editor">
          <Sidebar
            layout={layout}
            current={pageIndex}
            onSelect={(i) => (setPageIndex(i), setSelected(null))}
            onChange={setLayout}
          />
          <section class="stage">
            <Preview
              page={page}
              theme={layout.theme}
              selected={selected}
              onSelect={setSelected}
              onChange={updateWidget}
              onAddAt={(x, y) => setAdding({ x, y })}
            />
            <p class="muted small">Glisse un widget pour le déplacer, tire son coin pour le redimensionner, clique sur + pour en ajouter.</p>
          </section>
          <aside class="side">
            {widget ? (
              <Inspector widget={widget} page={page} entities={entities} onChange={updateWidget} onDelete={() => deleteWidget(widget.id)} />
            ) : (
              <p class="muted">Sélectionne un widget pour modifier ses propriétés.</p>
            )}
          </aside>
        </main>
      )}
      {tab === 'settings' && <SettingsView onError={(m) => fail(new ApiError(m === 'PIN incorrect' ? 401 : 0, m))} onSaved={notify} />}
      {tab === 'system' && <SystemView onError={(m) => fail(new ApiError(m === 'PIN incorrect' ? 401 : 0, m))} onSaved={notify} />}

      {adding && (
        <div class="modal" onClick={() => setAdding(null)}>
          <div class="modal-body" onClick={(e) => e.stopPropagation()}>
            <h3>Ajouter un widget</h3>
            <div class="catalog">
              {(Object.keys(CATALOG) as WidgetType[]).map((type) => (
                <button key={type} disabled={!CATALOG[type].available} onClick={() => addWidget(type)}>
                  <Icon name={CATALOG[type].icon} size={28} />
                  <span>{CATALOG[type].label}</span>
                  {!CATALOG[type].available && <small>bientôt</small>}
                </button>
              ))}
            </div>
          </div>
        </div>
      )}

      {toast && <div class={`toast ${toast.error ? 'error' : ''}`}>{toast.text}</div>}
    </div>
  );
}
