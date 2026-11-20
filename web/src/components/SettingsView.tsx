// Device settings: Wi-Fi, Home Assistant, weather location, time zone, display, PIN, paired PC, 3D printer.
import { useEffect, useState } from 'preact/hooks';

import { api, searchPlace, type Place } from '../api';
import { PRINTER_DRIVERS, PRINTER_MODELS } from '../generated/printers';
import type { PrinterState, Settings } from '../types';
import { ask } from './Dialog';

// French text of the printer states shown under the form
const PRINTER_STATES: Record<PrinterState['state'], string> = {
  off: 'non configurée',
  connecting: 'connexion…',
  offline: 'injoignable',
  idle: 'connectée, prête',
  printing: 'connectée, impression en cours',
  paused: 'connectée, en pause',
  finished: 'connectée, impression terminée',
  stopped: 'connectée, impression arrêtée',
  error: 'connectée, en erreur',
};

// Models grouped by brand, for the <optgroup>s of the picker
const PRINTER_BRANDS = [...new Set(PRINTER_MODELS.map((m) => m.brand))];

// POSIX time zones (the ESP32 has no tz database)
const TIMEZONES: [string, string][] = [
  ['Europe centrale (Paris, Bruxelles, Berlin…)', 'CET-1CEST,M3.5.0,M10.5.0/3'],
  ['Royaume-Uni, Portugal', 'GMT0BST,M3.5.0/1,M10.5.0'],
  ['Europe de l’Est (Athènes, Helsinki…)', 'EET-2EEST,M3.5.0/3,M10.5.0/4'],
  ['UTC', 'UTC0'],
  ['Québec, New York (Est)', 'EST5EDT,M3.2.0,M11.1.0'],
  ['Los Angeles (Pacifique)', 'PST8PDT,M3.2.0,M11.1.0'],
  ['La Réunion', '<+04>-4'],
  ['Martinique, Guadeloupe', 'AST4'],
  ['Nouvelle-Calédonie', '<+11>-11'],
  ['Tokyo', 'JST-9'],
];

/**
 * @description Settings form. Secrets are write-only: an empty field keeps the stored value.
 */
export function SettingsView({ onError, onSaved }: { onError: (message: string) => void; onSaved: (message: string) => void }) {
  const [settings, setSettings] = useState<Settings | null>(null);
  const [draft, setDraft] = useState<Settings | null>(null);
  const [query, setQuery] = useState('');
  const [places, setPlaces] = useState<Place[]>([]);
  const [printer, setPrinter] = useState<PrinterState | null>(null);

  useEffect(() => {
    api.getSettings().then((s) => (setSettings(s), setDraft(structuredClone(s))), (e) => onError(e.message));
  }, []);

  // Live connection state of the printer while the settings are open
  useEffect(() => {
    const poll = () => api.getPrinter().then(setPrinter, () => setPrinter(null));
    poll();
    const timer = setInterval(poll, 3000);
    return () => clearInterval(timer);
  }, []);

  if (!draft || !settings) return <div class="panel">Chargement…</div>;

  const set = <K extends keyof Settings>(key: K, patch: Partial<Settings[K]>) => setDraft({ ...draft, [key]: { ...draft[key], ...patch } });

  const save = async () => {
    try {
      const saved = await api.putSettings(draft);
      setSettings(saved);
      setDraft(structuredClone(saved));
      onSaved('Réglages enregistrés');
    } catch (e) {
      onError((e as Error).message);
    }
  };

  const forgetPc = async () => {
    const message = `Oublier « ${settings.pc.name || 'ce PC'} » ? Il faudra l’appairer à nouveau.`;
    if (!(await ask(message, { confirmLabel: 'Oublier', danger: true }))) return;
    try {
      const saved = await api.putSettings({ pc: { name: '', forget: true } });
      setSettings(saved);
      setDraft(structuredClone(saved));
      onSaved('PC oublié');
    } catch (e) {
      onError((e as Error).message);
    }
  };

  const search = async () => setPlaces(query.trim() ? await searchPlace(query.trim()) : []);
  const custom = !TIMEZONES.some(([, tz]) => tz === draft.time.timezone);

  return (
    <div class="settings">
      <section class="panel">
        <h3>Wi-Fi</h3>
        <label class="field">
          <span>Réseau (SSID)</span>
          <input value={draft.wifi.ssid} onInput={(e) => set('wifi', { ssid: e.currentTarget.value })} />
        </label>
        <label class="field">
          <span>Mot de passe {settings.wifi.has_password && <em>(enregistré)</em>}</span>
          <input type="password" placeholder={settings.wifi.has_password ? '••••••••' : ''} onInput={(e) => set('wifi', { password: e.currentTarget.value })} />
        </label>
        <small class="hint">L’écran se reconnecte dès l’enregistrement. Vérifie le nom du réseau : en cas d’erreur, il rouvre le Wi-Fi « DeskMonitor-setup ».</small>
      </section>

      <section class="panel">
        <h3>Home Assistant</h3>
        <label class="field">
          <span>Adresse</span>
          <input value={draft.ha.url} placeholder="http://homeassistant.local:8123" onInput={(e) => set('ha', { url: e.currentTarget.value })} />
        </label>
        <label class="field">
          <span>Jeton d’accès longue durée {settings.ha.has_token && <em>(enregistré)</em>}</span>
          <input type="password" placeholder={settings.ha.has_token ? '••••••••' : 'Profil HA → Sécurité → Jetons'} onInput={(e) => set('ha', { token: e.currentTarget.value })} />
        </label>
      </section>

      <section class="panel">
        <h3>Météo</h3>
        <p>
          Lieu actuel : <strong>{draft.weather.name}</strong>{' '}
          <span class="muted">
            ({draft.weather.lat.toFixed(3)}, {draft.weather.lon.toFixed(3)})
          </span>
        </p>
        <div class="row">
          <input placeholder="Rechercher une ville…" value={query} onInput={(e) => setQuery(e.currentTarget.value)} onKeyDown={(e) => e.key === 'Enter' && search()} />
          <button class="ghost" onClick={search}>
            Chercher
          </button>
        </div>
        {places.length > 0 && (
          <ul class="places">
            {places.map((p) => (
              <li key={`${p.latitude},${p.longitude}`}>
                <button
                  class="ghost"
                  onClick={() => (set('weather', { name: p.name, lat: p.latitude, lon: p.longitude }), setPlaces([]))}
                >
                  {p.name} <span class="muted">{[p.admin1, p.country].filter(Boolean).join(', ')}</span>
                </button>
              </li>
            ))}
          </ul>
        )}
        <label class="field">
          <span>Fuseau horaire</span>
          <select value={custom ? 'custom' : draft.time.timezone} onChange={(e) => e.currentTarget.value !== 'custom' && set('time', { timezone: e.currentTarget.value })}>
            {TIMEZONES.map(([label, tz]) => (
              <option key={tz} value={tz}>
                {label}
              </option>
            ))}
            <option value="custom">Personnalisé (POSIX)</option>
          </select>
        </label>
        {custom && <input value={draft.time.timezone} onInput={(e) => set('time', { timezone: e.currentTarget.value })} />}
      </section>

      <section class="panel">
        <h3>Écran</h3>
        <label class="field">
          <span>Luminosité : {draft.display.brightness} %</span>
          <input type="range" min={5} max={100} value={draft.display.brightness} onInput={(e) => set('display', { brightness: Number(e.currentTarget.value) })} />
        </label>
        <label class="field">
          <span>Mise en veille après (minutes, 0 = jamais)</span>
          <input type="number" min={0} max={240} value={draft.display.sleep_minutes} onInput={(e) => set('display', { sleep_minutes: Number(e.currentTarget.value) })} />
        </label>
      </section>

      <section class="panel">
        <h3>Ordinateur</h3>
        {settings.pc?.paired ? (
          <div class="row">
            <span>
              Appairé avec <strong>{settings.pc.name || 'un PC'}</strong>
            </span>
            <button class="ghost" onClick={forgetPc}>
              Oublier ce PC
            </button>
          </div>
        ) : (
          <small class="hint">
            Aucun PC appairé. Lance l’app Desk Monitor sur l’ordinateur : elle trouve l’écran sur le réseau, puis tu saisis le code
            affiché sur l’écran.
          </small>
        )}
      </section>

      <section class="panel">
        <h3>Imprimante 3D</h3>
        <label class="field">
          <span>Modèle</span>
          <select value={draft.printer.model} onChange={(e) => set('printer', { model: e.currentTarget.value })}>
            <option value="">Aucune imprimante</option>
            {PRINTER_BRANDS.map((brand) => (
              <optgroup key={brand} label={brand}>
                {PRINTER_MODELS.filter((m) => m.brand === brand).map((m) => (
                  <option key={m.id} value={m.id}>
                    {m.name}
                    {m.tested ? '' : ' (non testée)'}
                  </option>
                ))}
              </optgroup>
            ))}
          </select>
        </label>
        {draft.printer.model && (
          <>
            <div class="row">
              <label class="field">
                <span>Adresse IP de l’imprimante</span>
                <input value={draft.printer.host} placeholder="192.168.1.132" onInput={(e) => set('printer', { host: e.currentTarget.value.trim() })} />
              </label>
              <label class="field narrow">
                <span>Port</span>
                <input
                  type="number"
                  min={0}
                  max={65535}
                  value={draft.printer.port || ''}
                  placeholder={String(PRINTER_DRIVERS[PRINTER_MODELS.find((m) => m.id === draft.printer.model)?.driver ?? '']?.port ?? '')}
                  onInput={(e) => set('printer', { port: Number(e.currentTarget.value) || 0 })}
                />
              </label>
            </div>
            <small class="hint">
              {PRINTER_DRIVERS[PRINTER_MODELS.find((m) => m.id === draft.printer.model)?.driver ?? '']?.label}. L’adresse est
              affichée dans le menu réseau de l’imprimante ; fixe-la dans ta box pour qu’elle ne change pas.
            </small>
          </>
        )}
        {settings.printer.model && printer && (
          <p>
            État : <strong>{PRINTER_STATES[printer.state] ?? printer.state}</strong>
            {printer.name && printer.state !== 'off' && <span class="muted"> · {printer.name}</span>}
          </p>
        )}
      </section>

      <section class="panel">
        <h3>Sécurité</h3>
        <label class="field">
          <span>Code PIN de l’éditeur {settings.security.has_pin && <em>(actif)</em>}</span>
          <input type="password" inputMode="numeric" placeholder="Laisser vide pour ne pas changer" onInput={(e) => set('security', { pin: e.currentTarget.value })} />
        </label>
        {settings.security.has_pin && (
          <label class="check">
            <input type="checkbox" checked={!!draft.security.clear_pin} onChange={(e) => set('security', { clear_pin: e.currentTarget.checked })} /> Supprimer le PIN
          </label>
        )}
        <small class="hint">Le PIN est demandé pour toute modification (layout, réglages, mise à jour).</small>
      </section>

      <div class="actions">
        <button class="primary" onClick={save}>
          Enregistrer les réglages
        </button>
      </div>
    </div>
  );
}
