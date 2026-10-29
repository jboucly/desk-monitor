// In-page replacements for confirm() and prompt(). The companion opens the editor in a Tauri
// webview, where the native dialogs do nothing on macOS (confirm() returns false, prompt() null).
import { render } from 'preact';
import { useEffect, useRef } from 'preact/hooks';

/**
 * @description Options of a dialog.
 */
interface DialogOptions {
  /**
   * @description Label of the confirm button.
   */
  confirmLabel?: string;
  /**
   * @description Paints the confirm button red (destructive action).
   */
  danger?: boolean;
}

/**
 * @description Options of a dialog that asks for a text.
 */
interface TextOptions extends DialogOptions {
  /**
   * @description Hides the typed text (PIN).
   */
  secret?: boolean;
}

/**
 * @description Props of the dialog component.
 */
interface DialogProps {
  message: string;
  options: TextOptions;
  /**
   * @description Shows a text field when true.
   */
  input: boolean;
  /**
   * @description Called once: with the typed text (or '' without a field) on confirm, null on cancel.
   */
  onClose: (value: string | null) => void;
}

/**
 * @description Modal with a message, an optional text field, and cancel / confirm buttons.
 * Enter confirms, Escape and a click on the backdrop cancel.
 */
function Dialog({ message, options, input, onClose }: DialogProps) {
  const field = useRef<HTMLInputElement>(null);
  const confirmButton = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    (input ? field.current : confirmButton.current)?.focus();
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose(null);
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  return (
    <div class="modal" onClick={() => onClose(null)}>
      <form
        class="modal-body dialog"
        onClick={(e) => e.stopPropagation()}
        onSubmit={(e) => {
          e.preventDefault();
          onClose(field.current?.value ?? '');
        }}
      >
        <p>{message}</p>
        {input && <input ref={field} type={options.secret ? 'password' : 'text'} inputMode={options.secret ? 'numeric' : undefined} autocomplete="off" />}
        <div class="dialog-actions">
          <button type="button" class="ghost" onClick={() => onClose(null)}>
            Annuler
          </button>
          <button ref={confirmButton} type="submit" class={options.danger ? 'destructive' : 'primary'}>
            {options.confirmLabel ?? 'OK'}
          </button>
        </div>
      </form>
    </div>
  );
}

/**
 * @description Mounts a dialog outside the app tree and resolves when it closes.
 * @param message Text shown in the dialog.
 * @param options Labels and style.
 * @param input Whether the dialog asks for a text.
 * @returns The typed text ('' without a field), or null when cancelled.
 */
function open(message: string, options: TextOptions, input: boolean): Promise<string | null> {
  return new Promise((resolve) => {
    const host = document.body.appendChild(document.createElement('div'));
    const close = (value: string | null) => {
      render(null, host);
      host.remove();
      resolve(value);
    };
    render(<Dialog message={message} options={options} input={input} onClose={close} />, host);
  });
}

/**
 * @description Asks the user to confirm an action, like confirm().
 * @returns True when confirmed.
 * @example if (!(await ask('Supprimer la page ?', { confirmLabel: 'Supprimer', danger: true }))) return;
 */
export async function ask(message: string, options: DialogOptions = {}): Promise<boolean> {
  return (await open(message, options, false)) !== null;
}

/**
 * @description Asks the user for a text, like prompt().
 * @returns The typed text, or null when cancelled.
 */
export function askText(message: string, options: TextOptions = {}): Promise<string | null> {
  return open(message, options, true);
}
