// Service worker: держит соединение с локальным ядром и выполняет команды popup и ядра.
// Состояние (токен, магазин) хранится в chrome.storage.local — воркер MV3 может выгружаться в любой момент.
import { BridgeClient } from '../shared/bridge';
import type { ImportResult, PopupReply, PopupRequest, StatusReply } from '../shared/messages';

const CORE_URL = 'ws://127.0.0.1:8765/ws';
const KEEPALIVE_ALARM = 'dropship-keepalive';

interface Settings {
  token: string;
  storeId: number | null;
}

async function loadSettings(): Promise<Settings> {
  const s = await chrome.storage.local.get(['token', 'storeId']);
  return {
    token: typeof s.token === 'string' ? s.token : '',
    storeId: Number.isInteger(s.storeId) ? (s.storeId as number) : null,
  };
}

let client: BridgeClient | null = null;
let connecting: Promise<BridgeClient> | null = null;

async function ensureConnected(): Promise<BridgeClient> {
  if (client?.status === 'ready') return client;
  if (connecting) return connecting;
  connecting = (async () => {
    const { token } = await loadSettings();
    if (!token) throw new Error('Не задан токен доступа. Вставьте его в окне расширения.');
    const next = new BridgeClient({ url: CORE_URL, token, createSocket: (url) => new WebSocket(url) });
    next.onRequest = handleCoreRequest;
    try {
      await next.connect();
    } catch (e) {
      throw new Error(e instanceof Error ? e.message : String(e));
    }
    client = next;
    return next;
  })().finally(() => {
    connecting = null;
  });
  return connecting;
}

function handleCoreRequest(method: string): unknown {
  switch (method) {
    case 'PING':
      return { pong: true };
    case 'RELOAD_EXTENSION':
      // Ответ успевает уйти раньше перезагрузки.
      setTimeout(() => chrome.runtime.reload(), 200);
      return { reloading: true };
    default:
      throw Object.assign(new Error('method not found'), { code: -32601 });
  }
}

async function handle(request: PopupRequest): Promise<unknown> {
  switch (request.type) {
    case 'status': {
      const settings = await loadSettings();
      const reply: StatusReply = { connected: false, hasToken: settings.token !== '', storeId: settings.storeId };
      if (settings.token) {
        try {
          await ensureConnected();
          reply.connected = true;
        } catch (e) {
          reply.error = e instanceof Error ? e.message : String(e);
        }
      }
      return reply;
    }
    case 'save-settings': {
      const token = request.token.trim();
      if (!/^[0-9a-f]{32,}$/i.test(token)) throw new Error('Токен — это 64 символа из файла bridge.token');
      if (request.storeId !== null && (!Number.isInteger(request.storeId) || request.storeId < 1)) {
        throw new Error('Номер магазина должен быть целым числом от 1');
      }
      client?.close();
      client = null;
      await chrome.storage.local.set({ token, storeId: request.storeId });
      return {};
    }
    case 'import': {
      const { storeId } = await loadSettings();
      const bridge = await ensureConnected();
      const params = { ...request.product, ...(storeId !== null ? { store_id: storeId } : {}) };
      return (await bridge.call('IMPORT_PRODUCT_PAYLOAD', params)) as ImportResult;
    }
    case 'sync-cookies': {
      const url = new URL(request.pageUrl);
      const cookies = await chrome.cookies.getAll({ url: url.origin });
      const bridge = await ensureConnected();
      return bridge.call('SYNC_SUPPLIER_COOKIES', {
        domain: url.hostname,
        cookies: cookies.map((c) => ({ name: c.name, value: c.value })),
        user_agent: navigator.userAgent,
      });
    }
  }
}

chrome.runtime.onMessage.addListener((request: PopupRequest, sender, sendResponse) => {
  if (sender.id !== chrome.runtime.id) return false; // только собственные страницы расширения
  handle(request).then(
    (data): void => sendResponse({ ok: true, data } satisfies PopupReply),
    (e): void => sendResponse({ ok: false, error: e instanceof Error ? e.message : String(e) } satisfies PopupReply),
  );
  return true; // ответ асинхронный
});

// Будильник переподключает ядро, если воркер или соединение были выгружены.
chrome.runtime.onInstalled.addListener(() => void chrome.alarms.create(KEEPALIVE_ALARM, { periodInMinutes: 1 }));
chrome.runtime.onStartup.addListener(() => void chrome.alarms.create(KEEPALIVE_ALARM, { periodInMinutes: 1 }));
chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === KEEPALIVE_ALARM) void ensureConnected().catch(() => undefined);
});
