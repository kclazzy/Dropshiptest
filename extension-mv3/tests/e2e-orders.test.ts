// Сквозная проверка заказов: заказ -> команда расширению (ассистированный режим) -> результат -> трек.
// Выполняется, только если задан DROPSHIP_CORE_BIN.
import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import { createRequire } from 'node:module';
import { mkdtempSync, existsSync, readFileSync, writeFileSync, openSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import WebSocket from 'ws';
import { BridgeClient } from '../src/shared/bridge';
import { handleAutoOrder } from '../src/shared/orders';

const { DatabaseSync } = createRequire(import.meta.url)('node:sqlite');
const bin = process.env.DROPSHIP_CORE_BIN;
const port = 20000 + Math.floor(Math.random() * 20000);

describe.skipIf(!bin)('orders end to end', () => {
  let core: ChildProcess;
  let dir = '';

  beforeAll(async () => {
    dir = mkdtempSync(join(tmpdir(), 'dropship-e2e-orders-'));
    writeFileSync(join(dir, 'settings.json'), JSON.stringify({ ai: { provider: 'ollama' }, monitor: { workers: 0 } }));
    core = spawn(bin!, ['--data-dir', dir, '--port', String(port)], { stdio: ['ignore', 'ignore', openSync(join(dir, 'core.log'), 'a')] });
    for (let i = 0; i < 100 && !existsSync(join(dir, 'bridge.token')); i++) await new Promise((r) => setTimeout(r, 50));
    await new Promise((r) => setTimeout(r, 300));
  });
  afterAll(() => core?.kill('SIGTERM'));

  const rows = (sql: string): any[] => {
    const db = new DatabaseSync(join(dir, 'dropship.db'), { readOnly: true });
    try {
      return db.prepare(sql).all();
    } finally {
      db.close();
    }
  };
  const until = async (check: () => boolean, ms = 15_000) => {
    for (let t = 0; t < ms && !check(); t += 100) await new Promise((r) => setTimeout(r, 100));
    expect(check()).toBe(true);
  };

  it('buys through the extension, then ships and reports tracking', { timeout: 40_000 }, async () => {
    const token = readFileSync(join(dir, 'bridge.token'), 'utf8').trim();
    const opened: string[] = [];
    const c = new BridgeClient({ url: `ws://127.0.0.1:${port}/ws`, token, createSocket: (u) => new WebSocket(u) as any });
    c.onRequest = (method, params) => {
      if (method === 'EXECUTE_AUTO_ORDER') return handleAutoOrder(params, { openTab: async (u) => void opened.push(u), notify: async () => undefined });
      throw new Error('unexpected ' + method);
    };
    await c.connect();
    await c.call('IMPORT_PRODUCT_PAYLOAD', {
      url: 'https://supplier.example/item/1', title: 'Заказной товар', description: '', images: [],
      variants: [{ sku: 'S1', attributes: {}, price_cents: 9000, stock: 8 }],
    });
    const storeId = rows('SELECT store_id FROM products')[0].store_id;
    const variantId = rows('SELECT id FROM product_variants')[0].id;
    const w = new DatabaseSync(join(dir, 'dropship.db'));
    w.exec('PRAGMA busy_timeout = 5000');
    w.prepare('INSERT INTO pricing_rules(store_id, price_from_cents, price_to_cents, margin_percent, fixed_markup_cents, marketplace_fee_percent, rounding_cents, min_stock_threshold, min_profit_cents) VALUES (?, 0, 9223372036854775807, 0, 0, 10, NULL, 1, 200)').run(storeId);
    w.close();
    execFileSync(bin!, ['link', '--data-dir', dir, '--variant', String(variantId), '--external', 'OFFER-1']);

    const added = execFileSync(bin!, ['order-add', '--data-dir', dir, '--store', String(storeId), '--id', 'E2E-1', '--offer', 'OFFER-1', '--qty', '1', '--total', '150.00'], {
      input: JSON.stringify({ name: 'Иван', city: 'Тула' }),
    }).toString();
    expect(added).toContain('queued');

    await until(() => rows("SELECT status FROM orders")[0].status === 'QUEUED_FOR_EXT');
    expect(opened).toEqual(['https://supplier.example/item/1']);

    const list: any = await c.call('LIST_ORDERS', {});
    expect(list.orders[0]).toMatchObject({ external_order_id: 'E2E-1', sku: 'S1', address: { city: 'Тула' } });

    await c.call('ORDER_FULFILLMENT_RESULT', { order_id: list.orders[0].id, status: 'ordered', supplier_order_id: 'SUP-1' });
    await c.call('ORDER_TRACKING_UPDATE', { order_id: list.orders[0].id, tracking_number: 'TRK-9', carrier: 'CDEK' });
    c.close();
    await until(() => rows('SELECT status FROM task_queue WHERE task_type = \'TRACKING_SYNC\'').every((t: any) => t.status === 'COMPLETED') && rows('SELECT * FROM task_queue WHERE task_type = \'TRACKING_SYNC\'').length === 1);
    const order = rows('SELECT status, supplier_order_id, tracking_number, tracking_submitted_at FROM orders')[0];
    expect(order).toMatchObject({ status: 'SHIPPED', supplier_order_id: 'SUP-1', tracking_number: 'TRK-9' });
    expect(order.tracking_submitted_at).not.toBeNull();
  });
});
