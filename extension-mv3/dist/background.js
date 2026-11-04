// src/shared/bridge.ts
var RpcFailure = class extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
};
var BridgeClient = class {
  constructor(options) {
    this.options = options;
  }
  status = "disconnected";
  /** Токен, выданный ядром после успешной привязки по коду. */
  issuedToken = null;
  onStatus = () => {
  };
  onRequest = () => {
    throw new RpcFailure(-32601, "method not found");
  };
  socket = null;
  nextId = 1;
  pending = /* @__PURE__ */ new Map();
  heartbeat = null;
  /** Открывает соединение и ждёт успешной авторизации. */
  connect() {
    if (this.status === "ready") return Promise.resolve();
    if (this.socket) return Promise.reject(new Error("connection already in progress"));
    this.setStatus("connecting");
    return new Promise((resolve, reject) => {
      let socket;
      try {
        socket = this.options.createSocket(this.options.url);
      } catch (e) {
        this.setStatus("disconnected");
        reject(e instanceof Error ? e : new Error(String(e)));
        return;
      }
      this.socket = socket;
      let settled = false;
      const fail = (error) => {
        if (!settled) {
          settled = true;
          reject(error);
        }
      };
      socket.onopen = () => {
        this.setStatus("authenticating");
        const code = this.options.pairingCode;
        const first = code ? this.call("PAIR", { code }, this.options.authTimeoutMs ?? 5e3) : this.call("AUTH", { token: this.options.token }, this.options.authTimeoutMs ?? 5e3);
        first.then(
          (result) => {
            if (code) this.issuedToken = String(result?.token ?? "") || null;
            settled = true;
            this.setStatus("ready");
            this.startHeartbeat();
            resolve();
          },
          (e) => {
            fail(e);
            this.close();
          }
        );
      };
      socket.onmessage = (ev) => this.handleMessage(ev.data);
      socket.onerror = () => fail(new Error("\u041D\u0435 \u0443\u0434\u0430\u043B\u043E\u0441\u044C \u043F\u043E\u0434\u043A\u043B\u044E\u0447\u0438\u0442\u044C\u0441\u044F \u043A \u044F\u0434\u0440\u0443"));
      socket.onclose = () => {
        fail(new Error("\u0421\u043E\u0435\u0434\u0438\u043D\u0435\u043D\u0438\u0435 \u0437\u0430\u043A\u0440\u044B\u0442\u043E \u044F\u0434\u0440\u043E\u043C"));
        this.teardown();
      };
    });
  }
  call(method, params, timeoutMs = 1e4) {
    const socket = this.socket;
    if (!socket) return Promise.reject(new Error("\u041D\u0435\u0442 \u0441\u043E\u0435\u0434\u0438\u043D\u0435\u043D\u0438\u044F \u0441 \u044F\u0434\u0440\u043E\u043C"));
    if (method !== "AUTH" && method !== "PAIR" && this.status !== "ready") return Promise.reject(new Error("\u041D\u0435\u0442 \u0441\u043E\u0435\u0434\u0438\u043D\u0435\u043D\u0438\u044F \u0441 \u044F\u0434\u0440\u043E\u043C"));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`\u0422\u0430\u0439\u043C\u0430\u0443\u0442 \u043E\u0442\u0432\u0435\u0442\u0430 \u044F\u0434\u0440\u0430 (${method})`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      socket.send(JSON.stringify({ jsonrpc: "2.0", id, method, params }));
    });
  }
  close() {
    const socket = this.socket;
    this.teardown();
    try {
      socket?.close();
    } catch {
    }
  }
  teardown() {
    if (this.heartbeat) clearInterval(this.heartbeat);
    this.heartbeat = null;
    this.socket = null;
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new Error("\u0421\u043E\u0435\u0434\u0438\u043D\u0435\u043D\u0438\u0435 \u0441 \u044F\u0434\u0440\u043E\u043C \u043F\u043E\u0442\u0435\u0440\u044F\u043D\u043E"));
    }
    this.pending.clear();
    this.setStatus("disconnected");
  }
  startHeartbeat() {
    this.heartbeat = setInterval(() => {
      this.call("PING", {}, 5e3).catch(() => this.close());
    }, this.options.heartbeatMs ?? 2e4);
  }
  setStatus(status) {
    if (this.status === status) return;
    this.status = status;
    this.onStatus(status);
  }
  handleMessage(raw) {
    if (typeof raw !== "string") return;
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    if (msg === null || typeof msg !== "object") return;
    if (typeof msg.method === "string") {
      void this.answerRequest(msg);
      return;
    }
    const pending = typeof msg.id === "number" ? this.pending.get(msg.id) : void 0;
    if (!pending) return;
    this.pending.delete(msg.id);
    clearTimeout(pending.timer);
    if (msg.error) pending.reject(new RpcFailure(Number(msg.error.code), String(msg.error.message)));
    else pending.resolve(msg.result);
  }
  /** Запрос ядро -> расширение (например RELOAD_EXTENSION). */
  async answerRequest(msg) {
    if (msg.id === void 0 || msg.id === null) return;
    let reply;
    try {
      reply = { jsonrpc: "2.0", id: msg.id, result: await this.onRequest(msg.method, msg.params) };
    } catch (e) {
      const code = e instanceof RpcFailure ? e.code : -32603;
      reply = { jsonrpc: "2.0", id: msg.id, error: { code, message: e instanceof Error ? e.message : String(e) } };
    }
    this.socket?.send(JSON.stringify(reply));
  }
};

// src/shared/orders.ts
function parseAutoOrder(params) {
  const p = params;
  if (!p || typeof p !== "object") throw new RpcFailure(-32602, "params must be an object");
  const { order_id, url, sku, quantity, auto_pay } = p;
  if (!Number.isInteger(order_id) || order_id < 1) throw new RpcFailure(-32602, "bad order_id");
  if (!Number.isInteger(quantity) || quantity < 1) throw new RpcFailure(-32602, "bad quantity");
  if (typeof sku !== "string") throw new RpcFailure(-32602, "bad sku");
  if (auto_pay === true) throw new RpcFailure(-32602, "auto_pay is not supported by this extension");
  let parsed;
  try {
    parsed = new URL(String(url));
  } catch {
    throw new RpcFailure(-32602, "bad url");
  }
  if (parsed.protocol !== "https:" && parsed.protocol !== "http:") throw new RpcFailure(-32602, "bad url scheme");
  return { order_id, url: parsed.href, sku, quantity };
}
async function handleAutoOrder(params, deps) {
  const command = parseAutoOrder(params);
  await deps.openTab(command.url);
  await deps.notify(1);
  return { accepted: true, mode: "assisted" };
}

// src/background/service-worker.ts
var CORE_URL = "ws://127.0.0.1:8765/ws";
var KEEPALIVE_ALARM = "dropship-keepalive";
async function loadSettings() {
  const s = await chrome.storage.local.get(["token", "storeId"]);
  return {
    token: typeof s.token === "string" ? s.token : "",
    storeId: Number.isInteger(s.storeId) ? s.storeId : null
  };
}
var client = null;
var connecting = null;
async function ensureConnected() {
  if (client?.status === "ready") return client;
  if (connecting) return connecting;
  connecting = (async () => {
    const { token } = await loadSettings();
    if (!token) throw new Error("\u0420\u0430\u0441\u0448\u0438\u0440\u0435\u043D\u0438\u0435 \u043D\u0435 \u043F\u0440\u0438\u0432\u044F\u0437\u0430\u043D\u043E. \u041D\u0430\u0436\u043C\u0438\u0442\u0435 \xAB\u041F\u0440\u0438\u0432\u044F\u0437\u0430\u0442\u044C\xBB \u0438 \u0432\u0432\u0435\u0434\u0438\u0442\u0435 \u043A\u043E\u0434 \u0438\u0437 \u043F\u0440\u0438\u043B\u043E\u0436\u0435\u043D\u0438\u044F.");
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
function handleCoreRequest(method, params) {
  switch (method) {
    case "PING":
      return { pong: true };
    case "RELOAD_EXTENSION":
      setTimeout(() => chrome.runtime.reload(), 200);
      return { reloading: true };
    case "EXECUTE_AUTO_ORDER":
      return handleAutoOrder(params, {
        openTab: async (url) => void await chrome.tabs.create({ url, active: false }),
        notify: async () => {
          await chrome.action.setBadgeBackgroundColor({ color: "#d93025" });
          await chrome.action.setBadgeText({ text: "!" });
        }
      });
    default:
      throw Object.assign(new Error("method not found"), { code: -32601 });
  }
}
async function handle(request) {
  switch (request.type) {
    case "status": {
      const settings = await loadSettings();
      const reply = { connected: false, hasToken: settings.token !== "", storeId: settings.storeId };
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
    case "pair": {
      const code = request.code.trim();
      if (!/^[0-9a-f]{8}$/i.test(code)) throw new Error("\u041A\u043E\u0434 \u2014 8 \u0441\u0438\u043C\u0432\u043E\u043B\u043E\u0432 \u0438\u0437 \u043E\u043A\u043D\u0430 \xAB\u041F\u0440\u0438\u0432\u044F\u0437\u0430\u0442\u044C \u0440\u0430\u0441\u0448\u0438\u0440\u0435\u043D\u0438\u0435\xBB \u0432 \u043F\u0440\u0438\u043B\u043E\u0436\u0435\u043D\u0438\u0438");
      const pairing = new BridgeClient({ url: CORE_URL, token: "", pairingCode: code, createSocket: (url) => new WebSocket(url) });
      await pairing.connect();
      const token = pairing.issuedToken;
      pairing.close();
      if (!token) throw new Error("\u042F\u0434\u0440\u043E \u043D\u0435 \u0432\u044B\u0434\u0430\u043B\u043E \u0442\u043E\u043A\u0435\u043D. \u041F\u043E\u043B\u0443\u0447\u0438\u0442\u0435 \u043D\u043E\u0432\u044B\u0439 \u043A\u043E\u0434 \u0438 \u043F\u043E\u0432\u0442\u043E\u0440\u0438\u0442\u0435.");
      client?.close();
      client = null;
      await chrome.storage.local.set({ token });
      return {};
    }
    case "save-settings": {
      if (request.storeId !== null && (!Number.isInteger(request.storeId) || request.storeId < 1)) {
        throw new Error("\u041D\u043E\u043C\u0435\u0440 \u043C\u0430\u0433\u0430\u0437\u0438\u043D\u0430 \u0434\u043E\u043B\u0436\u0435\u043D \u0431\u044B\u0442\u044C \u0446\u0435\u043B\u044B\u043C \u0447\u0438\u0441\u043B\u043E\u043C \u043E\u0442 1");
      }
      await chrome.storage.local.set({ storeId: request.storeId });
      return {};
    }
    case "import": {
      const { storeId } = await loadSettings();
      const bridge = await ensureConnected();
      const params = { ...request.product, ...storeId !== null ? { store_id: storeId } : {} };
      return await bridge.call("IMPORT_PRODUCT_PAYLOAD", params);
    }
    case "orders-list":
      return (await ensureConnected()).call("LIST_ORDERS", {});
    case "order-result": {
      const bridge = await ensureConnected();
      return bridge.call("ORDER_FULFILLMENT_RESULT", {
        order_id: request.orderId,
        status: request.status,
        supplier_order_id: request.supplierOrderId ?? "",
        message: request.message ?? ""
      });
    }
    case "order-track":
      return (await ensureConnected()).call("ORDER_TRACKING_UPDATE", {
        order_id: request.orderId,
        tracking_number: request.trackingNumber,
        carrier: request.carrier ?? ""
      });
    case "update-check":
      return (await ensureConnected()).call("UPDATE_CHECK", {}, 2e4);
    case "update-apply":
      return (await ensureConnected()).call("UPDATE_APPLY", {});
    case "sync-cookies": {
      const url = new URL(request.pageUrl);
      const cookies = await chrome.cookies.getAll({ url: url.origin });
      const bridge = await ensureConnected();
      return bridge.call("SYNC_SUPPLIER_COOKIES", {
        domain: url.hostname,
        cookies: cookies.map((c) => ({ name: c.name, value: c.value })),
        user_agent: navigator.userAgent
      });
    }
  }
}
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  if (sender.id !== chrome.runtime.id) return false;
  handle(request).then(
    (data) => sendResponse({ ok: true, data }),
    (e) => sendResponse({ ok: false, error: e instanceof Error ? e.message : String(e) })
  );
  return true;
});
chrome.runtime.onInstalled.addListener(() => void chrome.alarms.create(KEEPALIVE_ALARM, { periodInMinutes: 1 }));
chrome.runtime.onStartup.addListener(() => void chrome.alarms.create(KEEPALIVE_ALARM, { periodInMinutes: 1 }));
chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === KEEPALIVE_ALARM) void ensureConnected().catch(() => void 0);
});
