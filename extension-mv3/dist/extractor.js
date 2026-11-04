"use strict";
(() => {
  // src/shared/extract.ts
  var DEFAULT_STOCK = 10;
  var MAX_IMAGES = 20;
  function parsePriceToCents(raw) {
    if (typeof raw === "number") {
      if (!Number.isFinite(raw) || raw < 0) return null;
      return Math.round(raw * 100);
    }
    if (typeof raw !== "string") return null;
    let s = raw.replace(/[^\d.,]/g, "");
    if (!/\d/.test(s)) return null;
    const lastSep = Math.max(s.lastIndexOf("."), s.lastIndexOf(","));
    let whole = s;
    let frac = "";
    if (lastSep >= 0) {
      const tail = s.slice(lastSep + 1);
      if (tail.length >= 1 && tail.length <= 2) {
        whole = s.slice(0, lastSep);
        frac = tail;
      }
    }
    whole = whole.replace(/[.,]/g, "");
    if (whole === "") whole = "0";
    const cents = Number(whole) * 100 + Number(frac.padEnd(2, "0") || "0");
    return Number.isSafeInteger(cents) ? cents : null;
  }
  function wellFormed(s) {
    return typeof s.toWellFormed === "function" ? s.toWellFormed() : s;
  }
  function text(v) {
    if (typeof v === "string") return wellFormed(v.trim());
    if (typeof v === "number") return String(v);
    return "";
  }
  function absoluteUrl(raw, base) {
    const value = typeof raw === "string" ? raw : raw && typeof raw === "object" ? raw.url : void 0;
    if (typeof value !== "string" || value.trim() === "") return null;
    try {
      const u = new URL(value.trim(), base);
      return u.protocol === "http:" || u.protocol === "https:" ? u.href : null;
    } catch {
      return null;
    }
  }
  function asArray(v) {
    return v === void 0 || v === null ? [] : Array.isArray(v) ? v : [v];
  }
  function* walk(node) {
    if (Array.isArray(node)) for (const n of node) yield* walk(n);
    else if (node && typeof node === "object") {
      yield node;
      yield* walk(node["@graph"]);
    }
  }
  function isType(node, type) {
    return asArray(node?.["@type"]).some((t) => t === type || String(t).endsWith("/" + type));
  }
  function jsonLdProducts(doc) {
    const found = [];
    for (const script of Array.from(doc.querySelectorAll('script[type="application/ld+json"]'))) {
      try {
        for (const node of walk(JSON.parse(script.textContent ?? ""))) if (isType(node, "Product")) found.push(node);
      } catch {
      }
    }
    return found;
  }
  function stockFromOffer(offer) {
    const level = offer?.inventoryLevel?.value ?? offer?.inventoryLevel;
    const n = typeof level === "string" ? Number(level) : level;
    if (typeof n === "number" && Number.isInteger(n) && n >= 0) return n;
    const availability = text(offer?.availability);
    if (/OutOfStock|SoldOut|Discontinued/i.test(availability)) return 0;
    return DEFAULT_STOCK;
  }
  function offerPrice(offer) {
    return parsePriceToCents(offer?.price ?? offer?.priceSpecification?.price ?? offer?.lowPrice);
  }
  function meta(doc, name) {
    const el = doc.querySelector(`meta[property="${name}"], meta[name="${name}"], meta[itemprop="${name}"]`);
    return text(el?.getAttribute("content"));
  }
  function extractProduct(doc, pageUrl) {
    const ld = jsonLdProducts(doc)[0];
    const title = text(ld?.name) || meta(doc, "og:title") || text(doc.title);
    if (!title) return { ok: false, error: "\u041D\u0435 \u043D\u0430\u0439\u0434\u0435\u043D \u0437\u0430\u0433\u043E\u043B\u043E\u0432\u043E\u043A \u0442\u043E\u0432\u0430\u0440\u0430" };
    const description = text(ld?.description) || meta(doc, "og:description") || meta(doc, "description");
    const images = [];
    for (const raw of [...asArray(ld?.image), meta(doc, "og:image")]) {
      const url = absoluteUrl(raw, pageUrl);
      if (url && !images.includes(url) && images.length < MAX_IMAGES) images.push(url);
    }
    const variants = [];
    for (const offer of asArray(ld?.offers).flatMap((o) => o?.offers ? asArray(o.offers) : [o])) {
      const price = offerPrice(offer);
      if (price === null) continue;
      const sku = text(offer?.sku) || text(ld?.sku);
      variants.push({ ...sku ? { sku } : {}, attributes: {}, price_cents: price, stock: stockFromOffer(offer) });
    }
    if (variants.length === 0) {
      const price = parsePriceToCents(meta(doc, "product:price:amount") || meta(doc, "og:price:amount") || meta(doc, "price"));
      if (price !== null) {
        const sku = text(ld?.sku);
        variants.push({ ...sku ? { sku } : {}, attributes: {}, price_cents: price, stock: DEFAULT_STOCK });
      }
    }
    if (variants.length === 0) {
      return { ok: false, error: "\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C \u043D\u0430\u0439\u0442\u0438 \u0446\u0435\u043D\u0443 \u043D\u0430 \u0441\u0442\u0440\u0430\u043D\u0438\u0446\u0435. \u041E\u0442\u043A\u0440\u043E\u0439\u0442\u0435 \u0441\u0442\u0440\u0430\u043D\u0438\u0446\u0443 \u043A\u043E\u043D\u043A\u0440\u0435\u0442\u043D\u043E\u0433\u043E \u0442\u043E\u0432\u0430\u0440\u0430." };
    }
    const productId = text(ld?.sku) || text(ld?.productID) || text(ld?.mpn);
    return {
      ok: true,
      product: {
        url: pageUrl,
        title,
        description,
        images,
        ...productId ? { supplier_product_id: productId } : {},
        variants
      }
    };
  }

  // src/content/extractor.ts
  globalThis.__dropshipExtract = () => extractProduct(document, location.href);
})();
