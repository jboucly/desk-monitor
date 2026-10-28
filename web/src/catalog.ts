// Widget catalog: labels, default sizes and props, and grid helpers.
import type { Layout, Page, Props, ThemeStyle, Widget, WidgetType } from './types';
import { GRID } from './types';

/**
 * @description Editor metadata of a widget type.
 */
export interface WidgetInfo {
  label: string;
  icon: string;
  w: number;
  h: number;
  /**
   * @description False for types the firmware does not render yet (PC companion, phase 2/3).
   */
  available: boolean;
  defaults: () => Props;
}

export const CATALOG: Record<WidgetType, WidgetInfo> = {
  clock: { label: 'Horloge', icon: 'clock-outline', w: 4, h: 2, available: true, defaults: () => ({ show_date: true }) },
  weather: { label: 'Météo', icon: 'weather-partly-cloudy', w: 2, h: 2, available: true, defaults: () => ({ mode: 'current' }) },
  ha_light: { label: 'Lumière HA', icon: 'lightbulb', w: 1, h: 1, available: true, defaults: () => ({ entity: '' }) },
  ha_toggle: { label: 'Interrupteur HA', icon: 'toggle-switch', w: 1, h: 1, available: true, defaults: () => ({ entity: '' }) },
  ha_sensor: { label: 'Capteur HA', icon: 'thermometer', w: 2, h: 1, available: true, defaults: () => ({ entity: '', decimals: 1 }) },
  ha_gauge: { label: 'Jauge HA', icon: 'gauge', w: 2, h: 2, available: true, defaults: () => ({ entity: '', min: 0, max: 100, unit: '%' }) },
  shortcut: {
    label: 'Raccourci',
    icon: 'rocket-launch',
    w: 1,
    h: 1,
    available: true,
    defaults: () => ({ name: 'Raccourci', icon: 'application', action: { kind: 'launch', target: '' } }),
  },
  pc_metrics: { label: 'Métriques PC', icon: 'desktop-tower', w: 4, h: 2, available: true, defaults: () => ({}) },
  pc_gauge: { label: 'Jauge PC', icon: 'cpu-64-bit', w: 2, h: 2, available: true, defaults: () => ({ metric: 'cpu' }) },
  media: { label: 'Musique', icon: 'music', w: 4, h: 2, available: true, defaults: () => ({}) },
};

/**
 * @description Metrics of the PC companion a pc_gauge can show (same list as the firmware).
 */
export const PC_METRICS: Record<string, { label: string; unit: string }> = {
  cpu: { label: 'Processeur', unit: '%' },
  ram: { label: 'Mémoire', unit: '%' },
  gpu: { label: 'GPU', unit: '%' },
  vram: { label: 'Mémoire GPU', unit: '%' },
  disk: { label: 'Disque', unit: '%' },
  cpu_temp: { label: 'Temp. CPU', unit: '°C' },
  gpu_temp: { label: 'Temp. GPU', unit: '°C' },
};

/**
 * @description Card styles offered in the editor, fastest first.
 */
export const STYLES: Record<ThemeStyle, string> = {
  flat: 'Sombre opaque (rapide)',
  frozen: 'Verre figé',
  glass: 'Verre translucide (lent)',
};

/**
 * @description Screen background of the flat style (same gradient as the device).
 */
export const FLAT_BACKGROUND = 'linear-gradient(#151926, #0b0d13)';

/**
 * @description Wallpaper presets with a CSS approximation of the device rendering.
 */
export const WALLPAPERS: Record<string, { label: string; css: (accent: string, accent2: string) => string }> = {
  aurora: {
    label: 'Aurore',
    css: (a, b) =>
      `radial-gradient(75% 75% at 10% 5%, ${a}8c, transparent 70%), radial-gradient(70% 70% at 100% 50%, ${b}73, transparent 70%), radial-gradient(65% 65% at 25% 105%, #2dd4bf66, transparent 70%), #070a14`,
  },
  nebula: {
    label: 'Nébuleuse',
    css: (a, b) =>
      `radial-gradient(70% 70% at 85% 5%, #e879f98c, transparent 70%), radial-gradient(70% 70% at 5% 55%, ${b}73, transparent 70%), radial-gradient(65% 65% at 70% 105%, ${a}66, transparent 70%), #0a0614`,
  },
  ocean: {
    label: 'Océan',
    css: () =>
      `radial-gradient(75% 75% at 15% 0%, #22d3ee80, transparent 70%), radial-gradient(70% 70% at 95% 60%, #3b82f673, transparent 70%), radial-gradient(60% 60% at 20% 105%, #14b8a659, transparent 70%), #030e18`,
  },
  sunset: {
    label: 'Coucher de soleil',
    css: (_a, b) =>
      `radial-gradient(75% 75% at 0% 10%, #fb923c80, transparent 70%), radial-gradient(70% 70% at 100% 35%, #f472b673, transparent 70%), radial-gradient(70% 70% at 50% 110%, ${b}66, transparent 70%), #120709`,
  },
  midnight: {
    label: 'Minuit',
    css: (a, b) => `radial-gradient(80% 80% at 20% 0%, ${a}47, transparent 70%), radial-gradient(80% 80% at 95% 100%, ${b}38, transparent 70%), #05070d`,
  },
};

/**
 * @description Default icon of an entity, chosen from its domain (same rules as the firmware).
 */
export function entityIcon(entity: string): string {
  const domain = entity.split('.')[0];
  const icons: Record<string, string> = {
    light: 'lightbulb',
    switch: 'power-socket-eu',
    fan: 'fan',
    climate: 'thermometer',
    cover: 'blinds',
    lock: 'lock',
    media_player: 'speaker',
    scene: 'palette',
    script: 'rocket-launch',
    automation: 'rocket-launch',
    input_boolean: 'toggle-switch',
    sensor: 'gauge',
  };
  return icons[domain] ?? 'home-assistant';
}

/**
 * @description Entity domains a widget type accepts in its picker.
 */
export const ENTITY_DOMAINS: Partial<Record<WidgetType, string[]>> = {
  ha_light: ['light'],
  ha_toggle: ['switch', 'light', 'fan', 'input_boolean', 'cover', 'lock', 'scene', 'script', 'automation', 'media_player'],
  ha_sensor: ['sensor', 'binary_sensor', 'input_number', 'number'],
  ha_gauge: ['sensor', 'input_number', 'number'],
};

/**
 * @description True when the rectangle fits the grid and does not overlap other widgets (except `ignore`).
 */
export function fits(page: Page, x: number, y: number, w: number, h: number, ignore?: string): boolean {
  if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > GRID || y + h > GRID) return false;
  return page.widgets.every((o) => o.id === ignore || x + w <= o.x || o.x + o.w <= x || y + h <= o.y || o.y + o.h <= y);
}

/**
 * @description First free spot for a widget of the given size, or null when the page is full.
 */
export function findSpot(page: Page, w: number, h: number): { x: number; y: number } | null {
  for (let y = 0; y < GRID; y++) for (let x = 0; x < GRID; x++) if (fits(page, x, y, w, h)) return { x, y };
  return null;
}

/**
 * @description Short unique id for a new widget or page.
 */
export function newId(prefix: string): string {
  return `${prefix}${Math.random().toString(36).slice(2, 7)}`;
}

/**
 * @description Creates a widget of a type at the first free spot (shrunk to 1x1 when needed).
 */
export function createWidget(page: Page, type: WidgetType): Widget | null {
  const info = CATALOG[type];
  for (const [w, h] of [[info.w, info.h], [Math.min(info.w, 2), Math.min(info.h, 2)], [1, 1]]) {
    const spot = findSpot(page, w, h);
    if (spot) return { id: newId('w'), type, ...spot, w, h, props: info.defaults() };
  }
  return null;
}

/**
 * @description Empty layout used before the first load.
 */
export const EMPTY_LAYOUT: Layout = {
  version: 1,
  theme: { accent: '#7C9CFF', accent2: '#C084FC', wallpaper: 'aurora', style: 'flat' },
  pages: [{ id: 'home', title: 'Accueil', widgets: [] }],
};
