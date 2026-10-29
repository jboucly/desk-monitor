// Companion window: shows the link to the screen, pairing, screen address, permissions, autostart.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);

/**
 * @description Label and dot color of each link status.
 */
const STATUS = {
  connecting: ['Connexion à l’écran…', 'warn'],
  connected: ['Connecté à l’écran', 'ok'],
  pair_required: ['Appairage nécessaire', 'warn'],
  pairing: ['Code affiché sur l’écran', 'warn'],
  offline: ['Écran injoignable', 'crit'],
};

/**
 * @description Renders the link state sent by the Rust side.
 * @param {{status: string, host: string, custom_host: string, paired: boolean, message: string}} state
 */
function render(state) {
  const [label, color] = STATUS[state.status] ?? [state.status, ''];
  $('status').textContent = label;
  $('dot').className = `dot ${color}`;
  $('message').textContent = state.message;
  $('host').textContent = state.host;
  if (document.activeElement !== $('host-input')) $('host-input').value = state.custom_host;
  $('pair-start').hidden = state.status !== 'pair_required';
  const pairing = state.status === 'pairing';
  if (pairing && $('pair-form').hidden) setTimeout(() => $('code').focus(), 0);
  $('pair-form').hidden = !pairing;
  $('forget-button').hidden = !state.paired;
}

/**
 * @description Shows the Accessibility card on macOS while key presses are not allowed.
 */
async function refreshAccessibility() {
  $('accessibility-card').hidden = await invoke('accessibility', { prompt: false });
}

$('pair-button').addEventListener('click', () => invoke('start_pairing'));

$('pair-form').addEventListener('submit', (event) => {
  event.preventDefault();
  const code = $('code').value.replace(/\D/g, '');
  if (code.length === 6) invoke('submit_code', { code });
  $('code').value = '';
});

$('host-form').addEventListener('submit', (event) => {
  event.preventDefault();
  invoke('set_host', { host: $('host-input').value });
  $('host-input').blur();
});

$('accessibility-button').addEventListener('click', async () => {
  await invoke('accessibility', { prompt: true });
  setTimeout(refreshAccessibility, 3000);
});

/**
 * @description Pending confirmation of the forget button. A native confirm() dialog would blur
 * the overlay, which hides it: the button asks for a second click instead.
 */
let forgetTimer = null;

/**
 * @description Puts the forget button back in its first state.
 */
function resetForget() {
  clearTimeout(forgetTimer);
  forgetTimer = null;
  $('forget-button').textContent = 'Oublier l’appairage';
}

$('forget-button').addEventListener('click', () => {
  if (forgetTimer) {
    resetForget();
    invoke('forget');
    return;
  }
  $('forget-button').textContent = 'Confirmer (nouveau code requis)';
  forgetTimer = setTimeout(resetForget, 4000);
});

document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') invoke('hide_overlay');
});

$('autostart').addEventListener('change', (event) => {
  invoke(event.target.checked ? 'plugin:autostart|enable' : 'plugin:autostart|disable');
});

listen('link-state', (event) => render(event.payload));
invoke('link_state').then(render);
invoke('plugin:autostart|is_enabled').then((enabled) => ($('autostart').checked = enabled));
refreshAccessibility();
window.addEventListener('focus', refreshAccessibility);
