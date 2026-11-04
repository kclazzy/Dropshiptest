// Сквозная проверка: импорт -> мониторинг -> расчёт цены -> отправка на «Ozon» (локальный двойник API).
// Выполняется, только если задан DROPSHIP_CORE_BIN.
import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import { createServer, type Server } from 'node:http';
import { createRequire } from 'node:module';
import { mkdtempSync, existsSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import WebSocket from 'ws';
import { BridgeClient } from '../src/shared/bridge';

const { DatabaseSync } = createRequire(import.meta.url)('node:sqlite');
const bin = process.env.DROPSHIP_CORE_BIN;
const port = 20000 + Math.floor(Math.random() * 20000);

describe.skipIf(!bin)('core -> Ozon', () => {
  let core: ChildProcess;
  let dir = '';
  let supplier: Server;
  let ozon: Server;
  const ozonCalls: { path: string; headers: Record<string, any>; body: any }[] = [];

  beforeAll(async () => {
    dir = mkdtempSync(join(tmpdir(), 'dropship-e2e-store-'));
    supplier = createServer((_req, res) => {
      res.setHeader('content-type', 'text/html');
      res.end('<script type="application/ld+json">{"@type":"Product","offers":{"sku":"S1","price":"100.00"}}</script>');
    });
    ozon = createServer((req, res) => {
      let raw = '';
      req.on('data', (c) => (raw += c));
      req.on('end', () => {
        const body = JSON.parse(raw || '{}');
        ozonCalls.push({ path: req.url!, headers: req.headers, body });
        res.setHeader('content-type', 'application/json');
        const rows = (body.stocks ?? body.prices ?? []).map((i: any) => ({ offer_id: i.offer_id, updated: true, errors: [] }));
        res.end(JSON.stringify({ result: rows }));
      });
    });
    await Promise.all([
      new Promise<void>((r) => supplier.listen(0, '127.0.0.1', r)),
      new Promise<void>((r) => ozon.listen(0, '127.0.0.1', r)),
    ]);
    const ozonPort = (ozon.address() as any).port;

    // Магазин добавляется командой, как у пользователя: ключи — через stdin.
    execFileSync(bin!, ['store-add', '--data-dir', dir, '--name', 'Тест Ozon', '--platform', 'ozon'], {
      input: JSON.stringify({ client_id: 'CID', api_key: 'KEY', warehouse_id: 5, base_url: `http://127.0.0.1:${ozonPort}` }),
    });
    execFileSync('node', ['-e', `
      const { DatabaseSync } = require('node:sqlite');
      const db = new DatabaseSync(${JSON.stringify(join(dir, 'dropship.db'))});
      db.prepare("INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) VALUES (1, 0, 9223372036854775807, 50, 0, 0, NULL, 1, 0)").run();
      db.close();`]);
    require_settings(dir);
    core = spawn(bin!, ['--data-dir', dir, '--port', String(port)], { stdio: ['ignore', 'ignore', require_log(dir)] });
    for (let i = 0; i < 100 && !existsSync(join(dir, 'bridge.token')); i++) await new Promise((r) => setTimeout(r, 50));
    await new Promise((r) => setTimeout(r, 300));
  });
  afterAll(() => {
    core?.kill('SIGTERM');
    supplier?.close();
    ozon?.close();
  });

  it('prices the product by the store rule and sends stock and price to Ozon', { timeout: 30_000 }, async () => {
    const token = readFileSync(join(dir, 'bridge.token'), 'utf8').trim();
    const c = new BridgeClient({ url: `ws://127.0.0.1:${port}/ws`, token, createSocket: (u) => new WebSocket(u) as any });
    await c.connect();
    const supplierPort = (supplier.address() as any).port;
    const imported: any = await c.call('IMPORT_PRODUCT_PAYLOAD', {
      url: `http://127.0.0.1:${supplierPort}/item`,
      title: 'Товар для Ozon',
      description: '',
      images: [],
      variants: [{ sku: 'S1', attributes: {}, price_cents: 9000, stock: 8 }],
    });
    c.close();
    expect(imported.duplicate).toBe(false);

    // Привязка варианта к offer_id в кабинете Ozon — командой.
    const db = new DatabaseSync(join(dir, 'dropship.db'), { readOnly: true });
    const variantId = (db.prepare('SELECT id FROM product_variants').get() as any).id;
    db.close();
    execFileSync(bin!, ['link', '--data-dir', dir, '--variant', String(variantId), '--external', 'OFFER-1']);

    for (let i = 0; i < 250 && !ozonCalls.some((x) => x.path === '/v1/product/import/prices'); i++) {
      await new Promise((r) => setTimeout(r, 100));
    }
    const stocks = ozonCalls.filter((x) => x.path === '/v2/products/stocks');
    const prices = ozonCalls.filter((x) => x.path === '/v1/product/import/prices');
    if (stocks.length === 0) console.error('CORE LOG', dir, readFileSync(join(dir, 'core.log'), 'utf8'), execFileSync(bin!, ['store-list', '--data-dir', dir]).toString());
    expect(stocks.length).toBeGreaterThan(0);
    expect(prices.length).toBeGreaterThan(0);
    // Последний отправленный набор: закупка 100.00 + 50 % = 150 ₽ (наценка по правилу магазина).
    const lastPrice = prices.at(-1)!;
    expect(lastPrice.body.prices[0]).toMatchObject({ offer_id: 'OFFER-1', price: '150' });
    expect(lastPrice.headers['client-id']).toBe('CID');
    expect(lastPrice.headers['api-key']).toBe('KEY');
    expect(stocks.at(-1)!.body.stocks[0]).toMatchObject({ offer_id: 'OFFER-1', stock: 10, warehouse_id: 5 });
  });
});

function require_settings(dir: string) {
  // Быстрые циклы планировщиков, чтобы тест не ждал по 15 секунд.
  const { writeFileSync } = createRequire(import.meta.url)('node:fs');
  writeFileSync(join(dir, 'settings.json'), JSON.stringify({ ai: { provider: 'ollama' }, monitor: { workers: 2, schedule_seconds: 1 } }));
}

function require_log(dir: string): number {
  // Журнал ядра сохраняется в файл: при сбое теста его видно (путь печатается в сообщении).
  const fs = createRequire(import.meta.url)('node:fs');
  return fs.openSync(join(dir, 'core.log'), 'a');
}
