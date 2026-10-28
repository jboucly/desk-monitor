// Shapes shared with the firmware. Source of truth: protocol/layout.schema.json and protocol/README.md.

/**
 * @description Widget types known by the firmware.
 */
export type WidgetType =
  | 'clock'
  | 'weather'
  | 'ha_toggle'
  | 'ha_light'
  | 'ha_sensor'
  | 'ha_gauge'
  | 'shortcut'
  | 'pc_metrics'
  | 'pc_gauge'
  | 'media';

/**
 * @description What a shortcut does: Home Assistant over Wi-Fi, or the PC companion over BLE.
 */
export type Action =
  | { kind: 'ha'; domain: string; service: string; entity_id?: string; data?: Record<string, unknown> }
  | { kind: 'keys'; keys: string }
  | { kind: 'launch'; target: string }
  | { kind: 'url'; url: string };

/**
 * @description Free-form props of a widget; each type reads the keys it knows.
 */
export type Props = Record<string, any>;

/**
 * @description A widget placed on the 4x4 grid of a page.
 */
export interface Widget {
  id: string;
  type: WidgetType;
  x: number;
  y: number;
  w: number;
  h: number;
  props: Props;
}

/**
 * @description One swipeable page of the screen.
 */
export interface Page {
  id: string;
  title: string;
  widgets: Widget[];
}

/**
 * @description Wallpaper presets generated on the device.
 */
export type Wallpaper = 'aurora' | 'nebula' | 'ocean' | 'sunset' | 'midnight';

/**
 * @description Card style: flat (opaque, no wallpaper, fastest), frozen (opaque, tinted like glass) or glass
 * (translucent, slowest on the device).
 */
export type ThemeStyle = 'flat' | 'frozen' | 'glass';

/**
 * @description Theme of the layout (accent colors, wallpaper and card style).
 */
export interface Theme {
  accent: string;
  accent2: string;
  wallpaper: Wallpaper;
  style: ThemeStyle;
}

/**
 * @description The whole layout.json document.
 */
export interface Layout {
  version: 1;
  theme: Theme;
  pages: Page[];
}

/**
 * @description Settings as returned by GET /api/settings (secrets replaced by has_* flags).
 * PUT accepts the same shape plus the secret fields; an empty secret keeps the stored one.
 */
export interface Settings {
  wifi: { ssid: string; password?: string; has_password?: boolean };
  ha: { url: string; token?: string; has_token?: boolean };
  weather: { lat: number; lon: number; name: string };
  time: { timezone: string };
  display: { brightness: number; sleep_minutes: number };
  security: { pin?: string; has_pin?: boolean; clear_pin?: boolean };
}

/**
 * @description Device status (GET /api/status).
 */
export interface Status {
  version: string;
  platform: string;
  ip: string;
  rssi?: number;
  uptime: number;
  heap_internal?: number;
  heap_psram?: number;
}

/**
 * @description Entry of the Home Assistant entity registry (GET /api/ha/entities).
 */
export interface HaEntity {
  id: string;
  name: string;
}

export const GRID = 4;
