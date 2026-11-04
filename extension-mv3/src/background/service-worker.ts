// Service worker: держит соединение с локальным ядром и выполняет команды popup и ядра.
// Состояние (токен, магазин) хранится в chrome.storage.local — воркер MV3 может выгружаться в любой момент.
import { BridgeClient } from '../shared/bridge';
import { handleAutoOrder } from '../shared/orders';
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
    if (!token) throw new Error('Расширение не привязано. Нажмите «Привязать» и введите код из приложения.');
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

function handleCoreRequest(method: string, params?: unknown): unknown {
  switch (method) {
    case 'PING':
      return { pong: true };
    case 'RELOAD_EXTENSION':
      // Ответ успевает уйти раньше перезагрузки.
      setTimeout(() => chrome.runtime.reload(), 200);
      return { reloading: true };
    case 'EXECUTE_AUTO_ORDER':
      return handleAutoOrder(params, {
        openTab: async (url) => void (await chrome.tabs.create({ url, active: false })),
        notify: async () => {
          await chrome.action.setBadgeBackgroundColor({ color: '#d93025' });
          await chrome.action.setBadgeText({ text: '!' });
        },
      });
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
    case 'pair': {
      const code = request.code.trim();
      if (!/^[0-9a-f]{8}$/i.test(code)) throw new Error('Код — 8 символов из окна «Привязать расширение» в приложении');
      const pairing = new BridgeClient({ url: CORE_URL, token: '', pairingCode: code, createSocket: (url) => new WebSocket(url) });
      await pairing.connect();
      const token = pairing.issuedToken;
      pairing.close();
      if (!token) throw new Error('Ядро не выдало токен. Получите новый код и повторите.');
      client?.close();
      client = null;
      await chrome.storage.local.set({ token });
      return {};
    }
    case 'save-settings': {
      if (request.storeId !== null && (!Number.isInteger(request.storeId) || request.storeId < 1)) {
        throw new Error('Номер магазина должен быть целым числом от 1');
      }
      await chrome.storage.local.set({ storeId: request.storeId });
      return {};
    }
    case 'import': {
      const { storeId } = await loadSettings();
      const bridge = await ensureConnected();
      const params = { ...request.product, ...(storeId !== null ? { store_id: storeId } : {}) };
      return (await bridge.call('IMPORT_PRODUCT_PAYLOAD', params)) as ImportResult;
    }
    case 'orders-list':
      return (await ensureConnected()).call('LIST_ORDERS', {});
    case 'order-result': {
      const bridge = await ensureConnected();
      return bridge.call('ORDER_FULFILLMENT_RESULT', {
        order_id: request.orderId,
        status: request.status,
        supplier_order_id: request.supplierOrderId ?? '',
        message: request.message ?? '',
      });
    }
    case 'order-track':
      return (await ensureConnected()).call('ORDER_TRACKING_UPDATE', {
        order_id: request.orderId,
        tracking_number: request.trackingNumber,
        carrier: request.carrier ?? '',
      });
    case 'update-check':
      return (await ensureConnected()).call('UPDATE_CHECK', {}, 20_000);
    case 'update-apply':
      return (await ensureConnected()).call('UPDATE_APPLY', {});
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
