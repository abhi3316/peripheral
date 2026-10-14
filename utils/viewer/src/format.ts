export function deg(v: number | null | undefined, digits = 2): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return '—';
  return `${v >= 0 ? '+' : '−'}${Math.abs(v).toFixed(digits)}°`;
}

export function bytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 ** 2) return `${(n / 1024).toFixed(1)} KiB`;
  if (n < 1024 ** 3) return `${(n / 1024 ** 2).toFixed(1)} MiB`;
  return `${(n / 1024 ** 3).toFixed(2)} GiB`;
}

// An age in microseconds of the server timebase, as text.
export function ageUs(us: number | null | undefined): string {
  if (us === null || us === undefined || !Number.isFinite(us)) return '—';
  const s = Math.max(0, us) / 1e6;
  if (s < 1) return `${Math.round(s * 1000)} ms`;
  if (s < 120) return `${s.toFixed(1)} s`;
  if (s < 7200) return `${Math.floor(s / 60)} min ${Math.round(s % 60)} s`;
  return `${(s / 3600).toFixed(1)} h`;
}

export function since(serverNowUs: number | null, atUs: number | null | undefined): string {
  if (serverNowUs === null || atUs === null || atUs === undefined) return '—';
  return `${ageUs(serverNowUs - atUs)} ago`;
}
