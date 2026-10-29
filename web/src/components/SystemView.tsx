// Device status, firmware update (OTA upload) and reboot.
import { useEffect, useState } from 'preact/hooks';

import { api } from '../api';
import type { Status } from '../types';
import { ask } from './Dialog';

/**
 * @description "2 j 03 h 12 min" from a number of seconds.
 */
function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  return `${d ? `${d} j ` : ''}${String(h).padStart(2, '0')} h ${String(m).padStart(2, '0')} min`;
}

/**
 * @description Status table, firmware upload with progress, reboot button.
 */
export function SystemView({ onError, onSaved }: { onError: (message: string) => void; onSaved: (message: string) => void }) {
  const [status, setStatus] = useState<Status | null>(null);
  const [progress, setProgress] = useState<number | null>(null);

  useEffect(() => {
    const load = () => api.getStatus().then(setStatus, () => setStatus(null));
    load();
    const timer = setInterval(load, 5000);
    return () => clearInterval(timer);
  }, []);

  const upload = async (file: File) => {
    const message = `Installer ${file.name} (${(file.size / 1024 / 1024).toFixed(2)} Mo) ? L’écran redémarrera.`;
    if (!(await ask(message, { confirmLabel: 'Installer' }))) return;
    try {
      setProgress(0);
      await api.uploadFirmware(file, setProgress);
      onSaved('Firmware installé, redémarrage…');
    } catch (e) {
      onError((e as Error).message);
    } finally {
      setProgress(null);
    }
  };

  const kib = (bytes?: number) => (bytes === undefined ? '—' : `${Math.round(bytes / 1024)} Kio`);

  return (
    <div class="settings">
      <section class="panel">
        <h3>État</h3>
        {status ? (
          <table class="status">
            <tbody>
              <tr><td>Firmware</td><td>{status.version}</td></tr>
              <tr><td>Plateforme</td><td>{status.platform}</td></tr>
              <tr><td>Adresse IP</td><td>{status.ip || '—'}</td></tr>
              {status.rssi !== undefined && <tr><td>Signal Wi-Fi</td><td>{status.rssi} dBm</td></tr>}
              <tr><td>Allumé depuis</td><td>{formatUptime(status.uptime)}</td></tr>
              <tr><td>Mémoire interne libre</td><td>{kib(status.heap_internal)}</td></tr>
              <tr><td>PSRAM libre</td><td>{kib(status.heap_psram)}</td></tr>
            </tbody>
          </table>
        ) : (
          <p class="muted">Écran injoignable.</p>
        )}
      </section>

      <section class="panel">
        <h3>Mise à jour du firmware</h3>
        <p class="muted">
          Choisis <code>firmware/build/desk_monitor.bin</code> (généré par <code>idf.py build</code>).
        </p>
        <input type="file" accept=".bin" disabled={progress !== null} onChange={(e) => e.currentTarget.files?.[0] && upload(e.currentTarget.files[0])} />
        {progress !== null && <progress max={1} value={progress} />}
      </section>

      <section class="panel">
        <h3>Redémarrage</h3>
        <button
          class="ghost"
          onClick={async () =>
            (await ask('Redémarrer l’écran ?', { confirmLabel: 'Redémarrer' })) && api.reboot().then(() => onSaved('Redémarrage…'), (e) => onError(e.message))
          }
        >
          Redémarrer l’écran
        </button>
      </section>
    </div>
  );
}
