// Сквозная проверка: настоящее ядро (C++) <-> настоящий BridgeClient расширения.
// Библиотека ws не шлёт Origin (как не-браузерный клиент); Node-овский WebSocket шлёт «null», и ядро его
// справедливо отклоняет. Выполняется, только если задан DROPSHIP_CORE_BIN (путь к dropship_core_app).
import { spawn, type ChildProcess } from 'node:child_process';
import { mkdtempSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import WebSocket from 'ws';
import { BridgeClient } from '../src/shared/bridge';

const bin = process.env.DROPSHIP_CORE_BIN;
const port = 20000 + Math.floor(Math.random() * 20000);

describe.skipIf(!bin)('extension <-> core', () => {
  let core: ChildProcess;
  let token = '';

  beforeAll(async () => {
    const dir = mkdtempSync(join(tmpdir(), 'dropship-e2e-'));
    core = spawn(bin!, ['--data-dir', dir, '--port', String(port)], { stdio: 'ignore' });
    for (let i = 0; i < 100 && !existsSync(join(dir, 'bridge.token')); i++) await new Promise((r) => setTimeout(r, 50));
    await new Promise((r) => setTimeout(r, 300));
    token = readFileSync(join(dir, 'bridge.token'), 'utf8').trim();
  });
  afterAll(() => core?.kill('SIGTERM'));

  const client = (t: string) =>
    new BridgeClient({ url: `ws://127.0.0.1:${port}/ws`, token: t, createSocket: (u) => new WebSocket(u) as any });

  it('rejects a wrong token', async () => {
    await expect(client('0'.repeat(64)).connect()).rejects.toThrow();
  });

  it('authenticates, imports a product and recognises the duplicate', async () => {
    const c = client(token);
    await c.connect();
    expect(await c.call('PING', {})).toMatchObject({ pong: true });
    const product = {
      url: 'https://shop.example/p/1?utm_source=x',
      title: 'Тестовый товар',
      description: 'desc',
      images: ['https://shop.example/a.jpg'],
      supplier_product_id: 'P1',
      variants: [{ sku: 'A', attributes: { color: 'Black' }, price_cents: 199950, stock: 10 }],
    };
    const first: any = await c.call('IMPORT_PRODUCT_PAYLOAD', product);
    expect(first).toMatchObject({ duplicate: false, variants: 1 });
    const second: any = await c.call('IMPORT_PRODUCT_PAYLOAD', { ...product, url: 'https://shop.example/p/1' });
    expect(second).toMatchObject({ duplicate: true, product_id: first.product_id });
    await expect(c.call('IMPORT_PRODUCT_PAYLOAD', { url: 'ftp://x', title: 'x', variants: [] })).rejects.toThrow();
    c.close();
  });

  it('stores supplier cookies', async () => {
    const c = client(token);
    await c.connect();
    const r: any = await c.call('SYNC_SUPPLIER_COOKIES', {
      domain: 'shop.example',
      cookies: [{ name: 'sid', value: 'abc' }],
      user_agent: 'UA',
    });
    expect(r.stored).toBe(1);
    c.close();
  });
});
