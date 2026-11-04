import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { BridgeClient, type WsLike } from '../src/shared/bridge';

class FakeSocket implements WsLike {
  onopen: any = null;
  onmessage: any = null;
  onclose: any = null;
  onerror: any = null;
  sent: any[] = [];
  closed = false;
  send(data: string) {
    this.sent.push(JSON.parse(data));
  }
  close() {
    this.closed = true;
    this.onclose?.({});
  }
  open() {
    this.onopen?.({});
  }
  reply(msg: unknown) {
    this.onmessage?.({ data: JSON.stringify(msg) });
  }
}

function setup(opts: { heartbeatMs?: number; authTimeoutMs?: number } = {}) {
  const socket = new FakeSocket();
  const client = new BridgeClient({ url: 'ws://x', token: 'tok', createSocket: () => socket, ...opts });
  return { socket, client };
}

async function authed(heartbeatMs?: number) {
  const s = setup({ heartbeatMs });
  const connecting = s.client.connect();
  s.socket.open();
  s.socket.reply({ jsonrpc: '2.0', id: s.socket.sent[0].id, result: { ok: true, protocol: 1 } });
  await connecting;
  return s;
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('BridgeClient', () => {
  it('sends AUTH with the token as the very first packet', async () => {
    const { socket, client } = await authed();
    expect(socket.sent[0]).toMatchObject({ method: 'AUTH', params: { token: 'tok' } });
    expect(client.status).toBe('ready');
  });

  it('pairs with a one-time code instead of AUTH and keeps the issued token', async () => {
    const socket = new FakeSocket();
    const client = new BridgeClient({ url: 'ws://x', token: '', pairingCode: 'AB12CD34', createSocket: () => socket });
    const connecting = client.connect();
    socket.open();
    expect(socket.sent[0]).toMatchObject({ method: 'PAIR', params: { code: 'AB12CD34' } });
    expect(socket.sent.some((m) => m.method === 'AUTH')).toBe(false);
    socket.reply({ jsonrpc: '2.0', id: socket.sent[0].id, result: { token: 'f'.repeat(64) } });
    await connecting;
    expect(client.issuedToken).toBe('f'.repeat(64));
    expect(client.status).toBe('ready');
  });

  it('a wrong pairing code rejects with the core message and issues no token', async () => {
    const socket = new FakeSocket();
    const client = new BridgeClient({ url: 'ws://x', token: '', pairingCode: 'BADBAD00', createSocket: () => socket });
    const connecting = client.connect();
    const assertion = expect(connecting).rejects.toThrow('код неверный');
    socket.open();
    socket.reply({ jsonrpc: '2.0', id: socket.sent[0].id, error: { code: -32001, message: 'код неверный или устарел' } });
    await assertion;
    expect(client.issuedToken).toBeNull();
  });

  it('rejects connect and closes when the core refuses the token', async () => {
    const { socket, client } = setup();
    const connecting = client.connect();
    const assertion = expect(connecting).rejects.toThrow();
    socket.open();
    socket.reply({ jsonrpc: '2.0', id: socket.sent[0].id, error: { code: -32003, message: 'unauthorized' } });
    await assertion;
    expect(socket.closed).toBe(true);
    expect(client.status).toBe('disconnected');
  });

  it('times out when the core never answers AUTH', async () => {
    const { socket, client } = setup({ authTimeoutMs: 100 });
    const assertion = expect(client.connect()).rejects.toThrow();
    socket.open();
    await vi.advanceTimersByTimeAsync(150);
    await assertion;
  });

  it('matches responses to calls by id and surfaces RPC errors', async () => {
    const { socket, client } = await authed();
    const ok = client.call('PING', {});
    const bad = client.call('IMPORT_PRODUCT_PAYLOAD', {});
    const badAssertion = expect(bad).rejects.toMatchObject({ code: -32602, message: 'bad' });
    socket.reply({ jsonrpc: '2.0', id: socket.sent[2].id, error: { code: -32602, message: 'bad' } });
    socket.reply({ jsonrpc: '2.0', id: socket.sent[1].id, result: { pong: true } });
    await expect(ok).resolves.toEqual({ pong: true });
    await badAssertion;
  });

  it('refuses calls before authentication and after close', async () => {
    const { client } = setup();
    await expect(client.call('PING', {})).rejects.toThrow();
    const ready = await authed();
    ready.client.close();
    await expect(ready.client.call('PING', {})).rejects.toThrow();
  });

  it('fails pending calls when the connection drops', async () => {
    const { socket, client } = await authed();
    const pending = client.call('PING', {});
    const assertion = expect(pending).rejects.toThrow();
    socket.close();
    await assertion;
    expect(client.status).toBe('disconnected');
  });

  it('sends PING heartbeats and drops the connection if the core stops answering', async () => {
    const { socket, client } = await authed(1000);
    await vi.advanceTimersByTimeAsync(1000);
    expect(socket.sent.at(-1)).toMatchObject({ method: 'PING' });
    await vi.advanceTimersByTimeAsync(5100);
    expect(client.status).toBe('disconnected');
  });

  it('answers requests from the core and reports unknown methods', async () => {
    const { socket, client } = await authed();
    client.onRequest = (method) => {
      if (method === 'ECHO') return { echoed: true };
      throw new Error('nope');
    };
    socket.reply({ jsonrpc: '2.0', id: 'c1', method: 'ECHO', params: {} });
    await vi.advanceTimersByTimeAsync(0);
    expect(socket.sent.at(-1)).toEqual({ jsonrpc: '2.0', id: 'c1', result: { echoed: true } });
    socket.reply({ jsonrpc: '2.0', id: 'c2', method: 'OTHER' });
    await vi.advanceTimersByTimeAsync(0);
    expect(socket.sent.at(-1)).toMatchObject({ id: 'c2', error: { code: -32603 } });
  });

  it('ignores garbage frames', async () => {
    const { socket, client } = await authed();
    socket.onmessage({ data: 'not json' });
    socket.onmessage({ data: 42 });
    socket.onmessage({ data: 'null' });
    expect(client.status).toBe('ready');
  });
});
