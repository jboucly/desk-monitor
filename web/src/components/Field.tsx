// Labeled form row shared by the property panels.

/**
 * @description Labeled form row.
 */
export function Field({ label, children }: { label: string; children: preact.ComponentChildren }) {
  return (
    <label class="field">
      <span>{label}</span>
      {children}
    </label>
  );
}
