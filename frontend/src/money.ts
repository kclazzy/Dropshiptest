// Суммы в копейках; в интерфейсе — рубли. Никаких float в расчётах: форматируем целые.
export const rub = (minor: number): string =>
  `${(minor / 100).toLocaleString('ru-RU', { minimumFractionDigits: 2, maximumFractionDigits: 2 })} ₽`;

export const toMinor = (rubles: number): number => Math.round(rubles * 100);
