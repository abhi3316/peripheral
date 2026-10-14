// A small seeded PRNG (mulberry32), so that fixtures and the mock repeat exactly.

export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Approximately normal, mean 0, standard deviation 1 (sum of 4 uniforms).
export function gaussian(rand: () => number): number {
  return (rand() + rand() + rand() + rand() - 2) * Math.sqrt(3);
}
