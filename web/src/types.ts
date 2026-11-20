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
  | 'ha_climate'
  | 'ha_cover'
  | 'ha_slider'
  | 'ha_select'
  | 'ha_list'
  | 'ha_history'
  | 'ha_alarm'
  | 'shortcut'
  | 'pc_metrics'
  | 'pc_gauge'
  | 'media'
  | 'claude_sessions'
  | 'claude_usage'
  | 'claude_limits'
  | 'printer';

/**
 * @description What a shortcut does: Home Assistant over Wi-Fi, or the paired PC companion over the local network.
 */
export type Action = (
  | { kind: 'ha'; domain: string; service: string; entity_id?: string; data?: Record<string, unknown> }
  | { kind: 'keys'; keys: string }
  | { kind: 'launch'; target: string }
  | { kind: 'url'; url: string }
  | { kind: 'toggle' | 'more_info'; entity_id?: string }
  | { kind: 'none' }
) & {
  /**
   * @description Asks before running: true for a default question, or the question to show.
   */
  confirm?: boolean | string;
};

/**
 * @description Gesture of a Home Assistant tile that the layout can override.
 */
export type Gesture = 'tap_action' | 'hold_action';

/**
 * @description Condition of a display rule. Every field set must hold; an empty condition always matches.
 * The tested value is the state of `entity` (default: the widget's entity), or its attribute `attr`.
 */
export interface RuleCondition {
  entity?: string;
  attr?: string;
  is?: string | string[];
  not?: string | string[];
  above?: number;
  below?: number;
}

/**
 * @description Display rule of a Home Assistant tile: the first one whose condition matches changes the
 * text (placeholders {state}, {name}, {attr.<name>}), the icon, the color or the active look.
 */
export interface Rule {
  when?: RuleCondition;
  text?: string;
  icon?: string;
  color?: string;
  active?: boolean;
}

/**
 * @description One entity (or template) of an ha_list, with its own name, icon, rules and actions.
 */
export interface ListItem {
  entity?: string;
  template?: string;
  name?: string;
  icon?: string;
  rules?: Rule[];
  tap_action?: Action;
  hold_action?: Action;
}

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
  pc: { name: string; paired?: boolean; forget?: boolean };
  /**
   * @description 3D printer: catalog id (protocol/printers.json, empty = none), address, port (0 = the driver's).
   */
  printer: { model: string; host: string; port: number };
}

/**
 * @description State of the 3D printer (GET /api/printer), the screen's "printer" bus value.
 */
export interface PrinterState {
  state: 'off' | 'connecting' | 'offline' | 'idle' | 'printing' | 'paused' | 'finished' | 'stopped' | 'error';
  model_name?: string;
  name?: string;
  progress?: number;
  nozzle?: number;
  bed?: number;
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
  /**
   * @description Why the screen last restarted (device only): "power_on", "panic", "brownout", "task_wdt"…
   */
  reset_reason?: string;
}

/**
 * @description Entry of the Home Assistant entity registry (GET /api/ha/entities).
 */
export interface HaEntity {
  id: string;
  name: string;
}

export const GRID = 4;
