import { ICONS } from '../generated/icons';

/**
 * @description Material Design icon from the set compiled into the firmware.
 */
export function Icon({ name, size = 20, color = 'currentColor' }: { name: string; size?: number; color?: string }) {
  const path = ICONS[name] ?? ICONS['alert-circle-outline'];
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" aria-hidden="true" style={{ flex: 'none' }}>
      <path d={path} fill={color} />
    </svg>
  );
}
