// @vitest-environment jsdom
import { describe, expect, it } from 'vitest';
import { extractProduct, parsePriceToCents } from '../src/shared/extract';

function page(head: string): Document {
  return new DOMParser().parseFromString(`<html><head><title>T</title>${head}</head><body></body></html>`, 'text/html');
}
const ld = (obj: unknown) => `<script type="application/ld+json">${JSON.stringify(obj)}</script>`;

describe('parsePriceToCents', () => {
  it.each([
    ['1 234,56 ₽', 123456],
    ['1,234.56', 123456],
    ['1.234,5', 123450],
    ['1234', 123400],
    ['1 200', 120000],
    ['19.99', 1999],
    ['0,99', 99],
    [19.9, 1990],
  ])('%s -> %s', (raw, cents) => expect(parsePriceToCents(raw)).toBe(cents));
  it.each([['abc'], [''], [-1], [NaN], [null]])('rejects %s', (raw) => expect(parsePriceToCents(raw)).toBeNull());
});

describe('extractProduct', () => {
  it('reads schema.org Product with several offers as variants', () => {
    const doc = page(
      ld({
        '@context': 'https://schema.org',
        '@type': 'Product',
        name: 'Наушники',
        description: 'Bluetooth 5.3',
        sku: 'P-1',
        image: ['/a.jpg', 'https://cdn.example/b.jpg', '/a.jpg', 'javascript:alert(1)'],
        offers: [
          { '@type': 'Offer', price: '1999.50', sku: 'P-1-B', availability: 'https://schema.org/InStock' },
          { '@type': 'Offer', price: 2100, sku: 'P-1-W', availability: 'https://schema.org/OutOfStock' },
          { '@type': 'Offer', price: '2200', sku: 'P-1-R', inventoryLevel: { value: 3 } },
        ],
      }),
    );
    const r = extractProduct(doc, 'https://shop.example/p/1');
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    expect(r.product.title).toBe('Наушники');
    expect(r.product.supplier_product_id).toBe('P-1');
    expect(r.product.images).toEqual(['https://shop.example/a.jpg', 'https://cdn.example/b.jpg']);
    expect(r.product.variants.map((v) => [v.sku, v.price_cents, v.stock])).toEqual([
      ['P-1-B', 199950, 10],
      ['P-1-W', 210000, 0],
      ['P-1-R', 220000, 3],
    ]);
  });

  it('finds Product inside @graph', () => {
    const doc = page(ld({ '@graph': [{ '@type': 'WebSite' }, { '@type': 'Product', name: 'X', offers: { price: '5' } }] }));
    const r = extractProduct(doc, 'https://s.example/x');
    expect(r.ok && r.product.variants[0]?.price_cents).toBe(500);
  });

  it('falls back to Open Graph and product meta tags', () => {
    const doc = page(
      '<meta property="og:title" content="OG товар"><meta property="og:image" content="/i.png">' +
        '<meta property="product:price:amount" content="99,90">',
    );
    const r = extractProduct(doc, 'https://s.example/p');
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.product.title).toBe('OG товар');
      expect(r.product.images).toEqual(['https://s.example/i.png']);
      expect(r.product.variants[0]).toMatchObject({ price_cents: 9990, stock: 10 });
    }
  });

  it('survives broken JSON-LD and reports a missing price', () => {
    const doc = page('<script type="application/ld+json">{oops</script><meta property="og:title" content="Без цены">');
    const r = extractProduct(doc, 'https://s.example/p');
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.error).toContain('цен');
  });

  it('strips lone surrogates so the core gets valid UTF-8', () => {
    const doc = page(ld({ '@type': 'Product', name: 'A\uD800B', offers: { price: '1' } }));
    const r = extractProduct(doc, 'https://s.example/p');
    expect(r.ok && r.product.title).toBe('A�B');
  });
});
