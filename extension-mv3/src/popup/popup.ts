import type { ExtractResult } from '../shared/extract';
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

function guarded(action: () => Promise<void>): () => void {
  return () => {
    action()
      .then(refreshStatus)
      .catch((e) => say(e instanceof Error ? e.message : String(e), true));
  };
}

$('update-apply').addEventListener('click', guarded(applyUpdate));
$('import').addEventListener('click', guarded(importCurrentPage));
$('cookies').addEventListener('click', guarded(syncCookies));
$('save').addEventListener('click', guarded(async () => {
  const storeText = $<HTMLInputElement>('store').value.trim();
  await send({
    type: 'save-settings',
    token: $<HTMLInputElement>('token').value,
    storeId: storeText === '' ? null : Number(storeText),
  });
  $<HTMLInputElement>('token').value = '';
  say('Сохранено.');
}));

refreshStatus().then(() => checkUpdate().catch(() => undefined)).catch((e) => say(e instanceof Error ? e.message : String(e), true));
