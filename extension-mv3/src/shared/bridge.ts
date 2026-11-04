// Клиент локального моста: WebSocket + JSON-RPC 2.0, первый пакет — AUTH с токеном
// (браузерный WebSocket не умеет заголовки). Не зависит от chrome.*, поэтому тестируется в Node.

export type BridgeStatus = 'disconnected' | 'connecting' | 'authenticating' | 'ready';

export interface WsLike {
  onopen: ((ev: any) => void) | null;
  onmessage: ((ev: any) => void) | null;
  onclose: ((ev: any) => void) | null;
  onerror: ((ev: any) => void) | null;
  send(data: string): void;
  close(): void;
}

export class RpcFailure extends Error {
  constructor(public code: number, message: string) {
    super(message);
  }
}

export interface BridgeOptions {
  url: string;
  token: string;
  /** Одноразовый код из приложения: вместо AUTH первым пакетом уходит PAIR, ядро выдаёт токен. */
  pairingCode?: string;
  createSocket: (url: string) => WsLike;
  heartbeatMs?: number;
  authTimeoutMs?: number;
}

type Pending = { resolve: (v: unknown) => void; reject: (e: Error) => void; timer: ReturnType<typeof setTimeout> };
export type RequestHandler = (method: string, params: unknown) => Promise<unknown> | unknown;

export class BridgeClient {
  status: BridgeStatus = 'disconnected';
  /** Токен, выданный ядром после успешной привязки по коду. */
  issuedToken: string | null = null;
  onStatus: (status: BridgeStatus) => void = () => {};
  onRequest: RequestHandler = () => {
    throw new RpcFailure(-32601, 'method not found');
  };

  private socket: WsLike | null = null;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private heartbeat: ReturnType<typeof setInterval> | null = null;

  constructor(private readonly options: BridgeOptions) {}

  /** Открывает соединение и ждёт успешной авторизации. */
  connect(): Promise<void> {
    if (this.status === 'ready') return Promise.resolve();
    if (this.socket) return Promise.reject(new Error('connection already in progress'));
    this.setStatus('connecting');
    return new Promise<void>((resolve, reject) => {
      let socket: WsLike;
      try {
        socket = this.options.createSocket(this.options.url);
      } catch (e) {
        this.setStatus('disconnected');
        reject(e instanceof Error ? e : new Error(String(e)));
        return;
      }
      this.socket = socket;
      let settled = false;
      const fail = (error: Error) => {
        if (!settled) {
          settled = true;
          reject(error);
        }
      };
      socket.onopen = () => {
        this.setStatus('authenticating');
        const code = this.options.pairingCode;
        const first = code ? this.call('PAIR', { code }, this.options.authTimeoutMs ?? 5000) : this.call('AUTH', { token: this.options.token }, this.options.authTimeoutMs ?? 5000);
        first.then(
          (result) => {
            if (code) this.issuedToken = String((result as { token?: unknown } | null)?.token ?? '') || null;
            settled = true;
            this.setStatus('ready');
            this.startHeartbeat();
            resolve();
          },
          (e: Error) => {
            fail(e);
            this.close();
          },
        );
      };
      socket.onmessage = (ev) => this.handleMessage(ev.data);
      socket.onerror = () => fail(new Error('Не удалось подключиться к ядру'));
      socket.onclose = () => {
        fail(new Error('Соединение закрыто ядром'));
        this.teardown();
      };
    });
  }

  call(method: string, params: unknown, timeoutMs = 10_000): Promise<unknown> {
    const socket = this.socket;
    if (!socket) return Promise.reject(new Error('Нет соединения с ядром'));
    if (method !== 'AUTH' && method !== 'PAIR' && this.status !== 'ready') return Promise.reject(new Error('Нет соединения с ядром'));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Таймаут ответа ядра (${method})`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      socket.send(JSON.stringify({ jsonrpc: '2.0', id, method, params }));
    });
  }

  close(): void {
    const socket = this.socket;
    this.teardown();
    try {
      socket?.close();
    } catch {
      /* уже закрыт */
    }
  }

  private teardown(): void {
    if (this.heartbeat) clearInterval(this.heartbeat);
    this.heartbeat = null;
    this.socket = null;
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(new Error('Соединение с ядром потеряно'));
    }
    this.pending.clear();
    this.setStatus('disconnected');
  }

  private startHeartbeat(): void {
    this.heartbeat = setInterval(() => {
      this.call('PING', {}, 5000).catch(() => this.close());
    }, this.options.heartbeatMs ?? 20_000);
  }

  private setStatus(status: BridgeStatus): void {
    if (this.status === status) return;
    this.status = status;
    this.onStatus(status);
  }

  private handleMessage(raw: unknown): void {
    if (typeof raw !== 'string') return;
    let msg: any;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    if (msg === null || typeof msg !== 'object') return;
    if (typeof msg.method === 'string') {
      void this.answerRequest(msg);
      return;
    }
    const pending = typeof msg.id === 'number' ? this.pending.get(msg.id) : undefined;
    if (!pending) return;
    this.pending.delete(msg.id);
    clearTimeout(pending.timer);
    if (msg.error) pending.reject(new RpcFailure(Number(msg.error.code), String(msg.error.message)));
    else pending.resolve(msg.result);
  }

  /** Запрос ядро -> расширение (например RELOAD_EXTENSION). */
  private async answerRequest(msg: { id?: unknown; method: string; params?: unknown }): Promise<void> {
    if (msg.id === undefined || msg.id === null) return; // уведомления не требуют ответа
    let reply: unknown;
    try {
      reply = { jsonrpc: '2.0', id: msg.id, result: await this.onRequest(msg.method, msg.params) };
    } catch (e) {
      const code = e instanceof RpcFailure ? e.code : -32603;
      reply = { jsonrpc: '2.0', id: msg.id, error: { code, message: e instanceof Error ? e.message : String(e) } };
    }
    this.socket?.send(JSON.stringify(reply));
  }
}
