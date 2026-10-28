// Left column of the editor: pages (add, rename, reorder, delete) and theme.
import { STYLES, WALLPAPERS, newId } from '../catalog';
import type { Layout, Page, Theme, ThemeStyle, Wallpaper } from '../types';
import { Icon } from './Icon';

interface SidebarProps {
  layout: Layout;
  current: number;
  onSelect: (index: number) => void;
  onChange: (layout: Layout) => void;
}

/**
 * @description Page list and theme controls.
 */
export function Sidebar({ layout, current, onSelect, onChange }: SidebarProps) {
  const setPages = (pages: Page[]) => onChange({ ...layout, pages });
  const setTheme = (patch: Partial<Theme>) => onChange({ ...layout, theme: { ...layout.theme, ...patch } });

  const move = (index: number, delta: number) => {
    const pages = [...layout.pages];
    const [page] = pages.splice(index, 1);
    pages.splice(index + delta, 0, page);
    setPages(pages);
    onSelect(index + delta);
  };

  const add = () => {
    if (layout.pages.length >= 12) return;
    setPages([...layout.pages, { id: newId('p'), title: 'Nouvelle page', widgets: [] }]);
    onSelect(layout.pages.length);
  };

  const remove = (index: number) => {
    if (layout.pages.length <= 1 || !confirm(`Supprimer la page « ${layout.pages[index].title} » ?`)) return;
    setPages(layout.pages.filter((_, i) => i !== index));
    onSelect(Math.max(0, index - 1));
  };

  return (
    <aside class="sidebar">
      <h3>Pages</h3>
      <ul class="pages">
        {layout.pages.map((page, i) => (
          <li key={page.id} class={i === current ? 'active' : ''} onClick={() => onSelect(i)}>
            <input
              value={page.title}
              maxLength={32}
              onInput={(e) => setPages(layout.pages.map((p, j) => (j === i ? { ...p, title: e.currentTarget.value } : p)))}
            />
            <span class="page-actions">
              <button title="Monter" disabled={i === 0} onClick={(e) => (e.stopPropagation(), move(i, -1))}>
                <Icon name="chevron-up" size={16} />
              </button>
              <button title="Descendre" disabled={i === layout.pages.length - 1} onClick={(e) => (e.stopPropagation(), move(i, 1))}>
                <Icon name="chevron-down" size={16} />
              </button>
              <button title="Supprimer" disabled={layout.pages.length <= 1} onClick={(e) => (e.stopPropagation(), remove(i))}>
                <Icon name="close" size={16} />
              </button>
            </span>
          </li>
        ))}
      </ul>
      <button class="ghost" disabled={layout.pages.length >= 12} onClick={add}>
        <Icon name="plus" size={16} /> Ajouter une page
      </button>

      <h3>Thème</h3>
      <label class="field">
        <span>Style</span>
        <select value={layout.theme.style} onChange={(e) => setTheme({ style: e.currentTarget.value as ThemeStyle })}>
          {Object.entries(STYLES).map(([key, label]) => (
            <option key={key} value={key}>
              {label}
            </option>
          ))}
        </select>
      </label>
      {layout.theme.style !== 'flat' && (
        <label class="field">
          <span>Fond d’écran</span>
          <select value={layout.theme.wallpaper} onChange={(e) => setTheme({ wallpaper: e.currentTarget.value as Wallpaper })}>
            {Object.entries(WALLPAPERS).map(([key, w]) => (
              <option key={key} value={key}>
                {w.label}
              </option>
            ))}
          </select>
        </label>
      )}
      <div class="row">
        <label class="field">
          <span>Accent</span>
          <input type="color" value={layout.theme.accent} onInput={(e) => setTheme({ accent: e.currentTarget.value.toUpperCase() })} />
        </label>
        <label class="field">
          <span>Accent 2</span>
          <input type="color" value={layout.theme.accent2} onInput={(e) => setTheme({ accent2: e.currentTarget.value.toUpperCase() })} />
        </label>
      </div>
    </aside>
  );
}
