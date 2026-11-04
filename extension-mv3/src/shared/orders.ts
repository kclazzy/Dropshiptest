// Режим «ассистированной покупки»: ядро присылает команду EXECUTE_AUTO_ORDER, расширение открывает страницу
// поставщика, а человек сам оформляет и оплачивает заказ. Автозаполнение и автооплата НЕ реализованы:
// покупка — необратимое действие, ошибка стоит денег.
import { RpcFailure } from './bridge';

export interface AutoOrderCommand {
  order_id: number;
  url: string;
  sku: string;
  quantity: number;
}

export interface OrderRow {
  id: number;
  external_order_id: string;
  status: string;
  quantity: number;
  retail_total_cents: number;
  net_profit_cents: number | null;
  address: Record<string, unknown>;
  supplier_order_id: string | null;
  tracking_number: string | null;
  error_message: string | null;
  supplier_url: string | null;
  sku: string | null;
  title: string | null;
}

export function parseAutoOrder(params: unknown): AutoOrderCommand {
  const p = params as Record<string, unknown> | null;
  if (!p || typeof p !== 'object') throw new RpcFailure(-32602, 'params must be an object');
  const { order_id, url, sku, quantity, auto_pay } = p;
  if (!Number.isInteger(order_id) || (order_id as number) < 1) throw new RpcFailure(-32602, 'bad order_id');
  if (!Number.isInteger(quantity) || (quantity as number) < 1) throw new RpcFailure(-32602, 'bad quantity');
  if (typeof sku !== 'string') throw new RpcFailure(-32602, 'bad sku');
  if (auto_pay === true) throw new RpcFailure(-32602, 'auto_pay is not supported by this extension');
  let parsed: URL;
  try {
    parsed = new URL(String(url));
  } catch {
    throw new RpcFailure(-32602, 'bad url');
  }
  if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') throw new RpcFailure(-32602, 'bad url scheme');
  return { order_id: order_id as number, url: parsed.href, sku, quantity: quantity as number };
}

export interface OrderDeps {
  openTab(url: string): Promise<void>;
  notify(pending: number): Promise<void>;
}

export async function handleAutoOrder(params: unknown, deps: OrderDeps): Promise<{ accepted: true; mode: 'assisted' }> {
  const command = parseAutoOrder(params);
  await deps.openTab(command.url);
  await deps.notify(1);
  return { accepted: true, mode: 'assisted' };
}

// Заказы, ожидающие действия человека (для значка и списка в окне расширения).
export function needsAction(order: Pick<OrderRow, 'status'>): boolean {
  return ['QUEUED_FOR_EXT', 'MANUAL_REVIEW', 'ERROR', 'ORDERED'].includes(order.status);
}

export function addressLines(address: Record<string, unknown>): string {
  return Object.entries(address)
    .filter(([, v]) => v !== null && v !== '' && typeof v !== 'object')
    .map(([k, v]) => `${k}: ${String(v)}`)
    .join('\n');
}
