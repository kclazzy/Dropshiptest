// Извлечение товара со страницы: JSON-LD (schema.org Product) -> Open Graph -> meta.
// Чистая функция над Document: работает на любом магазине, а не только на одном сайте.

export interface VariantPayload {
  sku?: string;
  attributes: Record<string, string>;
  price_cents: number;
  stock: number;
}

export interface ProductPayload {
  url: string;
  title: string;
  description: string;
  images: string[];
  supplier_product_id?: string;
  variants: VariantPayload[];
}

export type ExtractResult = { ok: true; product: ProductPayload } | { ok: false; error: string };

const DEFAULT_STOCK = 10; // «в наличии» без количества
const MAX_IMAGES = 20;

/** "1 234,56 ₽" | "1,234.56" | 19.9 -> копейки (целое) или null. Без плавающей арифметики. */
export function parsePriceToCents(raw: unknown): number | null {
  if (typeof raw === 'number') {
    if (!Number.isFinite(raw) || raw < 0) return null;
    return Math.round(raw * 100);
  }
  if (typeof raw !== 'string') return null;
  let s = raw.replace(/[^\d.,]/g, '');
  if (!/\d/.test(s)) return null;
  const lastSep = Math.max(s.lastIndexOf('.'), s.lastIndexOf(','));
  let whole = s;
  let frac = '';
  if (lastSep >= 0) {
    const tail = s.slice(lastSep + 1);
    if (tail.length >= 1 && tail.length <= 2) {
      whole = s.slice(0, lastSep);
      frac = tail;
    }
  }
  whole = whole.replace(/[.,]/g, '');
  if (whole === '') whole = '0';
  const cents = Number(whole) * 100 + Number(frac.padEnd(2, '0') || '0');
  return Number.isSafeInteger(cents) ? cents : null;
}

function wellFormed(s: string): string {
  return typeof s.toWellFormed === 'function' ? s.toWellFormed() : s;
}

function text(v: unknown): string {
  if (typeof v === 'string') return wellFormed(v.trim());
  if (typeof v === 'number') return String(v);
  return '';
}

function absoluteUrl(raw: unknown, base: string): string | null {
  const value = typeof raw === 'string' ? raw : raw && typeof raw === 'object' ? (raw as any).url : undefined;
  if (typeof value !== 'string' || value.trim() === '') return null;
  try {
    const u = new URL(value.trim(), base);
    return u.protocol === 'http:' || u.protocol === 'https:' ? u.href : null;
  } catch {
    return null;
  }
}

function asArray<T>(v: T | T[] | undefined | null): T[] {
  return v === undefined || v === null ? [] : Array.isArray(v) ? v : [v];
}

function* walk(node: unknown): Generator<any> {
  if (Array.isArray(node)) for (const n of node) yield* walk(n);
  else if (node && typeof node === 'object') {
    yield node;
    yield* walk((node as any)['@graph']);
  }
}

function isType(node: any, type: string): boolean {
  return asArray<string>(node?.['@type']).some((t) => t === type || String(t).endsWith('/' + type));
}

function jsonLdProducts(doc: Document): any[] {
  const found: any[] = [];
  for (const script of Array.from(doc.querySelectorAll('script[type="application/ld+json"]'))) {
    try {
      for (const node of walk(JSON.parse(script.textContent ?? ''))) if (isType(node, 'Product')) found.push(node);
    } catch {
      /* битый JSON-LD пропускаем */
    }
  }
  return found;
}

function stockFromOffer(offer: any): number {
  const level = offer?.inventoryLevel?.value ?? offer?.inventoryLevel;
  const n = typeof level === 'string' ? Number(level) : level;
  if (typeof n === 'number' && Number.isInteger(n) && n >= 0) return n;
  const availability = text(offer?.availability);
  if (/OutOfStock|SoldOut|Discontinued/i.test(availability)) return 0;
  return DEFAULT_STOCK;
}

function offerPrice(offer: any): number | null {
  return parsePriceToCents(offer?.price ?? offer?.priceSpecification?.price ?? offer?.lowPrice);
}

function meta(doc: Document, name: string): string {
  const el = doc.querySelector(`meta[property="${name}"], meta[name="${name}"], meta[itemprop="${name}"]`);
  return text(el?.getAttribute('content'));
}

export function extractProduct(doc: Document, pageUrl: string): ExtractResult {
  const ld = jsonLdProducts(doc)[0];
  const title = text(ld?.name) || meta(doc, 'og:title') || text(doc.title);
  if (!title) return { ok: false, error: 'Не найден заголовок товара' };

  const description = text(ld?.description) || meta(doc, 'og:description') || meta(doc, 'description');

  const images: string[] = [];
  for (const raw of [...asArray(ld?.image), meta(doc, 'og:image')]) {
    const url = absoluteUrl(raw, pageUrl);
    if (url && !images.includes(url) && images.length < MAX_IMAGES) images.push(url);
  }

  const variants: VariantPayload[] = [];
  for (const offer of asArray<any>(ld?.offers).flatMap((o) => (o?.offers ? asArray(o.offers) : [o]))) {
    const price = offerPrice(offer);
    if (price === null) continue;
    const sku = text(offer?.sku) || text(ld?.sku);
    variants.push({ ...(sku ? { sku } : {}), attributes: {}, price_cents: price, stock: stockFromOffer(offer) });
  }
  if (variants.length === 0) {
    const price = parsePriceToCents(meta(doc, 'product:price:amount') || meta(doc, 'og:price:amount') || meta(doc, 'price'));
    if (price !== null) {
      const sku = text(ld?.sku);
      variants.push({ ...(sku ? { sku } : {}), attributes: {}, price_cents: price, stock: DEFAULT_STOCK });
    }
  }
  if (variants.length === 0) {
    return { ok: false, error: 'Не удалось найти цену на странице. Откройте страницу конкретного товара.' };
  }

  const productId = text(ld?.sku) || text(ld?.productID) || text(ld?.mpn);
  return {
    ok: true,
    product: {
      url: pageUrl,
      title,
      description,
      images,
      ...(productId ? { supplier_product_id: productId } : {}),
      variants,
    },
  };
}
