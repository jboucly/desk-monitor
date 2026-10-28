// Client of the screen's REST API (served by the screen itself, or proxied to the simulator in dev).
import type { HaEntity, Layout, Settings, Status } from './types';

const PIN_KEY = 'dm-pin';

/**
 * @description Error carrying the HTTP status and the French message sent by the firmware.
 */
export class ApiError extends Error {
  constructor(
    public readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

/**
 * @description PIN remembered for this browser tab (sent as X-DM-Pin on every write).
 */
export function getPin(): string {
  return sessionStorage.getItem(PIN_KEY) ?? '';
}

/**
 * @description Stores the PIN for the next requests of this tab.
 */
export function setPin(pin: string): void {
  sessionStorage.setItem(PIN_KEY, pin);
}

/**
 * @description Sends a request and decodes the JSON answer, throwing ApiError on failure.
 */
async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const response = await fetch(path, {
    method,
    headers: { 'Content-Type': 'application/json', 'X-DM-Pin': getPin() },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new ApiError(response.status, data.error ?? `Erreur HTTP ${response.status}`);
  return data as T;
}

export const api = {
  getLayout: () => request<Layout>('GET', '/api/layout'),
  putLayout: (layout: Layout) => request<{ ok: true }>('PUT', '/api/layout', layout),
  getSettings: () => request<Settings>('GET', '/api/settings'),
  putSettings: (settings: Partial<Settings>) => request<Settings>('PUT', '/api/settings', settings),
  getStatus: () => request<Status>('GET', '/api/status'),
  getEntities: () => request<HaEntity[]>('GET', '/api/ha/entities'),
  reboot: () => request<{ ok: true }>('POST', '/api/reboot'),

  /**
   * @description Uploads a firmware image (build/desk_monitor.bin) and reports the progress (0-1).
   */
  uploadFirmware(file: File, onProgress: (ratio: number) => void): Promise<void> {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.setRequestHeader('X-DM-Pin', getPin());
      xhr.upload.onprogress = (e) => e.lengthComputable && onProgress(e.loaded / e.total);
      xhr.onload = () => {
        if (xhr.status === 200) return resolve();
        let message = `Erreur HTTP ${xhr.status}`;
        try {
          message = JSON.parse(xhr.responseText).error ?? message;
        } catch {
          // Not JSON: keep the generic message
        }
        reject(new ApiError(xhr.status, message));
      };
      xhr.onerror = () => reject(new ApiError(0, 'Connexion perdue pendant l’envoi'));
      xhr.send(file);
    });
  },
};

/**
 * @description City found by the Open-Meteo geocoding API.
 */
export interface Place {
  name: string;
  latitude: number;
  longitude: number;
  country?: string;
  admin1?: string;
}

/**
 * @description Searches a city by name (called from the browser, the screen is not involved).
 */
export async function searchPlace(query: string): Promise<Place[]> {
  const url = `https://geocoding-api.open-meteo.com/v1/search?count=6&language=fr&format=json&name=${encodeURIComponent(query)}`;
  const response = await fetch(url);
  if (!response.ok) return [];
  const data = await response.json();
  return data.results ?? [];
}
