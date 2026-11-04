import type { ExtractResult } from '../shared/extract';
import { addressLines, needsAction, type OrderRow } from '../shared/orders';
import type { ImportResult, PopupReply, PopupRequest, StatusReply, UpdateInfo } from '../shared/messages';

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

function say(text: string, error = false): void {
  const el = $('message');
  el.textContent = text;
  el.className = error ? 'error' : '';
}

async function send<T>(request: PopupRequest): Promise<T> {
  const reply = (await chrome.runtime.sendMessage(request)) as PopupReply<T> | undefined;
  if (!reply) throw new Error('Расширение не отвечает');
  if (!reply.ok) throw new Error(reply.error);
  return reply.data;
}

async function activeTab(): Promise<chrome.tabs.Tab> {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab?.id || !tab.url || !/^https?:/.test(tab.url)) throw new Error('Откройте страницу товара (http/https)');
  return tab;
}

async function refreshStatus(): Promise<void> {
  const status = await send<StatusReply>({ type: 'status' });
  const el = $('status');
  el.className = status.connected ? 'status ok' : 'status';
  el.textContent = status.connected
    ? 'Связь с ядром установлена'
    : !status.hasToken
      ? 'Вставьте токен доступа в настройках'
      : `Нет связи с ядром. ${status.error ?? 'Запущено ли приложение?'}`;
  if (!status.hasToken) ($('settings') as HTMLDetailsElement).open = true;
  if (status.storeId !== null) $<HTMLInputElement>('store').value = String(status.storeId);
}

async function importCurrentPage(): Promise<void> {
  const tab = await activeTab();
  // Скрипт внедряется только сейчас, по клику (activeTab); на сайтах ничего не висит постоянно.
  await chrome.scripting.executeScript({ target: { tabId: tab.id! }, files: ['extractor.js'] });
  const [injected] = await chrome.scripting.executeScript({
    target: { tabId: tab.id! },
    func: () => (globalThis as any).__dropshipExtract() as ExtractResult,
  });
  const result = injected?.result as ExtractResult | undefined;
  if (!result) throw new Error('Не удалось прочитать страницу');
  if (!result.ok) throw new Error(result.error);
  const saved = await send<ImportResult>({ type: 'import', product: result.product });
  say(
    saved.duplicate
      ? `Этот товар уже добавлен (№${saved.product_id}). Данные обновлены.`
      : `Добавлено: «${result.product.title}»\nВариантов: ${saved.variants}. ИИ готовит описание.`,
  );
}

async function syncCookies(): Promise<void> {
  const tab = await activeTab();
  const origin = new URL(tab.url!).origin + '/*';
  // Разрешения запрашиваются по требованию и только для этого сайта.
  const granted = await chrome.permissions.request({ origins: [origin], permissions: ['cookies'] });
  if (!granted) throw new Error('Без разрешения на куки сессию передать нельзя');
  await send({ type: 'sync-cookies', pageUrl: tab.url! });
  say('Сессия сайта передана в ядро.');
}

async function checkUpdate(): Promise<void> {
  const info = await send<UpdateInfo>({ type: 'update-check' });
  $('update').hidden = !info.update_available;
  if (!info.update_available) return;
  $('update-text').textContent = `Доступна версия ${info.latest} (у вас ${info.current}).`;
  const apply = $<HTMLButtonElement>('update-apply');
  apply.hidden = !info.can_apply;
  if (!info.can_apply) $('update-text').textContent += ' Запустите ядро через run.sh / run.cmd, чтобы обновляться кнопкой.';
}

async function applyUpdate(): Promise<void> {
  await send({ type: 'update-apply' });
  say('Обновляю… Ядро перезапустится само, расширение перезагрузится через минуту.');
  // Расширение собирается в ту же папку, поэтому перезагрузка подхватывает новые файлы.
  setTimeout(() => chrome.runtime.reload(), 60_000);
}

const STATUS_TEXT: Record<string, string> = {
  NEW: 'Ожидает отправки',
  QUEUED_FOR_EXT: 'Нужно выкупить у поставщика',
  ORDERED: 'Выкуплен — укажите трек',
  SHIPPED: 'Трек передан',
  MANUAL_REVIEW: 'Требует вашего решения',
  ERROR: 'Ошибка выкупа',
};

function el<K extends keyof HTMLElementTagNameMap>(tag: K, text = '', className = ''): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  node.textContent = text; // только textContent: данные заказа приходят извне
  if (className) node.className = className;
  return node;
}

function actionRow(placeholder: string, button: string, onClick: (value: string) => Promise<void>): HTMLElement {
  const row = el('div', '', 'row');
  const input = el('input');
  input.placeholder = placeholder;
  const go = el('button', button);
  go.addEventListener('click', guarded(() => onClick(input.value.trim())));
  row.append(input, go);
  return row;
}

function renderOrder(order: OrderRow): HTMLElement {
  const card = el('div', '', 'order');
  card.append(el('div', `№${order.external_order_id} · ${order.title ?? 'товар не привязан'} × ${order.quantity}`));
  card.append(el('div', STATUS_TEXT[order.status] ?? order.status, 'st'));
  if (order.error_message) card.append(el('div', order.error_message, 'note'));
  const address = addressLines(order.address);
  if (address && ['QUEUED_FOR_EXT', 'MANUAL_REVIEW', 'ERROR', 'NEW'].includes(order.status)) {
    card.append(el('pre', address));
    const row = el('div', '', 'row');
    const copy = el('button', 'Скопировать адрес');
    copy.addEventListener('click', () => void navigator.clipboard.writeText(address).then(() => say('Адрес скопирован.')));
    row.append(copy);
    if (order.supplier_url && /^https?:/.test(order.supplier_url)) {
      const open = el('button', `Открыть поставщика (арт. ${order.sku ?? '?'})`);
      open.addEventListener('click', () => void chrome.tabs.create({ url: order.supplier_url! }));
      row.append(open);
    }
    card.append(row);
  }
  if (['QUEUED_FOR_EXT', 'MANUAL_REVIEW', 'ERROR', 'NEW'].includes(order.status)) {
    card.append(actionRow('№ заказа у поставщика', 'Выкуплено', async (value) => {
      if (!value) throw new Error('Введите номер заказа у поставщика');
      await send({ type: 'order-result', orderId: order.id, status: 'ordered', supplierOrderId: value });
      say('Заказ отмечен как выкупленный.');
    }));
    if (order.status === 'QUEUED_FOR_EXT') {
      const fail = el('button', 'Не удалось выкупить');
      fail.addEventListener('click', guarded(async () => {
        await send({ type: 'order-result', orderId: order.id, status: 'failed', message: 'Отмечено вручную: выкуп не удался' });
        say('Заказ отмечен как неудавшийся.');
      }));
      card.append(fail);
    }
  }
  if (['ORDERED', 'SHIPPED'].includes(order.status)) {
    card.append(actionRow(order.tracking_number ?? 'Трек-номер', 'Отправить трек', async (value) => {
      if (!value) throw new Error('Введите трек-номер');
      await send({ type: 'order-track', orderId: order.id, trackingNumber: value });
      say('Трек сохранён, ядро передаст его в магазин.');
    }));
  }
  return card;
}

async function loadOrders(): Promise<void> {
  const reply = await send<{ orders: OrderRow[] }>({ type: 'orders-list' });
  const box = $('orders');
  box.replaceChildren();
  const active = reply.orders.filter((o) => needsAction(o));
  if (active.length === 0) box.append(el('div', 'Заказов, требующих действий, нет.'));
  for (const order of active) box.append(renderOrder(order));
  const waiting = reply.orders.filter((o) => o.status === 'QUEUED_FOR_EXT').length;
  await chrome.action.setBadgeText({ text: waiting > 0 ? String(waiting) : '' });
}

function guarded(action: () => Promise<void>): () => void {
  return () => {
    action()
      .then(refreshStatus)
      .then(() => loadOrders().catch(() => undefined))
      .catch((e) => say(e instanceof Error ? e.message : String(e), true));
  };
}

$('orders-refresh').addEventListener('click', guarded(loadOrders));
$('update-apply').addEventListener('click', guarded(applyUpdate));
$('import').addEventListener('click', guarded(importCurrentPage));
$('cookies').addEventListener('click', guarded(syncCookies));
$('save').addEventListener('click', guarded(async () => {
  const storeText = $<HTMLInputElement>('store').value.trim();
  await send({
    type: 'save-settings',
    storeId: storeText === '' ? null : Number(storeText),
  });
  say('Сохранено.');
}));
$('pair').addEventListener('click', guarded(async () => {
  await send({ type: 'pair', code: $<HTMLInputElement>('code').value });
  $<HTMLInputElement>('code').value = '';
  say('Расширение привязано.');
  await refreshStatus();
}));

refreshStatus().then(() => loadOrders().catch(() => undefined)).then(() => checkUpdate().catch(() => undefined)).catch((e) => say(e instanceof Error ? e.message : String(e), true));
