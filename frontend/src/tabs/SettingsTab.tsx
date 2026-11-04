import { useCallback, useEffect, useState } from 'react';
import { call, type AiState, type Rule, type StoreRow, type UpdateInfo } from '../api';
import { Button, Card, Field, inputCls, Pill, rub, toMinor, useAction } from '../ui';

export const PROVIDERS: { id: string; name: string; keyUrl: string; needsKey: boolean }[] = [
  { id: 'ollama', name: 'Ollama (на вашем компьютере, бесплатно)', keyUrl: 'https://ollama.com/download', needsKey: false },
  { id: 'openai', name: 'OpenAI', keyUrl: 'https://platform.openai.com/api-keys', needsKey: true },
  { id: 'anthropic', name: 'Anthropic (Claude)', keyUrl: 'https://console.anthropic.com/settings/keys', needsKey: true },
  { id: 'gemini', name: 'Google Gemini', keyUrl: 'https://aistudio.google.com/app/apikey', needsKey: true },
  { id: 'openrouter', name: 'OpenRouter', keyUrl: 'https://openrouter.ai/keys', needsKey: true },
  { id: 'groq', name: 'Groq', keyUrl: 'https://console.groq.com/keys', needsKey: true },
  { id: 'together', name: 'Together AI', keyUrl: 'https://api.together.ai/settings/api-keys', needsKey: true },
  { id: 'mistral', name: 'Mistral', keyUrl: 'https://console.mistral.ai/api-keys', needsKey: true },
  { id: 'deepseek', name: 'DeepSeek', keyUrl: 'https://platform.deepseek.com/api_keys', needsKey: true },
  { id: 'custom', name: 'Другой сервис (совместимый с OpenAI)', keyUrl: '', needsKey: false },
];

export function AiForm({ onSaved }: { onSaved?: () => void }) {
  const run = useAction();
  const [state, setState] = useState<AiState | null>(null);
  const [provider, setProvider] = useState('ollama');
  const [model, setModel] = useState('');
  const [baseUrl, setBaseUrl] = useState('');
  const [key, setKey] = useState('');

  useEffect(() => {
    void run(() => call<AiState>('get_ai_settings')).then((s) => {
      if (!s) return;
      setState(s);
      setProvider(s.settings.provider);
      setModel(s.settings.model ?? '');
      setBaseUrl(s.settings.base_url ?? '');
    });
  }, [run]);

  const info = PROVIDERS.find((p) => p.id === provider);
  return (
    <div>
      <Field label="Какой ИИ пишет карточки товаров">
        <select value={provider} onChange={(e) => setProvider(e.target.value)} className={inputCls}>
          {PROVIDERS.map((p) => (
            <option key={p.id} value={p.id}>
              {p.name}
            </option>
          ))}
        </select>
      </Field>
      {info?.keyUrl && (
        <p className="mb-3 text-sm">
          {info.needsKey ? 'Ключ можно создать здесь: ' : 'Установка: '}
          <code>{info.keyUrl}</code> — подробности в <code>docs/AI_PROVIDERS.md</code>.
        </p>
      )}
      <div className="grid gap-x-4 sm:grid-cols-2">
        <Field label="Модель" note="Пусто — модель по умолчанию для сервиса.">
          <input value={model} onChange={(e) => setModel(e.target.value)} className={inputCls} />
        </Field>
        {(provider === 'custom' || provider === 'ollama') && (
          <Field label="Адрес сервиса" note={provider === 'ollama' ? 'Обычно http://127.0.0.1:11434 — менять не нужно.' : 'Например http://127.0.0.1:1234/v1'}>
            <input value={baseUrl} onChange={(e) => setBaseUrl(e.target.value)} className={inputCls} />
          </Field>
        )}
        {(info?.needsKey || provider === 'custom') && (
          <Field label="Ключ API" note={state?.has_key ? 'Ключ сохранён. Оставьте пустым, чтобы не менять; впишите «-», чтобы удалить.' : 'Хранится зашифрованным на этом компьютере.'}>
            <input type="password" autoComplete="off" value={key} onChange={(e) => setKey(e.target.value)} className={inputCls} />
          </Field>
        )}
      </div>
      <Button
        kind="primary"
        onClick={async () => {
          const apiKey = key === '' ? null : key === '-' ? '' : key;
          const ok = await run(() => call('save_ai_settings', { settings: { provider, model: model.trim() || null, base_url: baseUrl.trim() || null, language: 'ru' }, apiKey }), 'Настройки ИИ сохранены');
          if (ok !== undefined) {
            setKey('');
            onSaved?.();
          }
        }}
      >
        Сохранить
      </Button>
    </div>
  );
}

export function PairingBox() {
  const run = useAction();
  const [code, setCode] = useState('');
  const [paired, setPaired] = useState(false);
  const load = useCallback(async () => {
    const s = await run(() => call<{ paired: boolean }>('extension_status'));
    if (s) setPaired(s.paired);
  }, [run]);
  useEffect(() => {
    void load();
  }, [load]);
  return (
    <div className="space-y-3 text-sm">
      <p>
        Статус: {paired ? <Pill tone="good">расширение привязано</Pill> : <Pill tone="warn">не привязано</Pill>}
      </p>
      <ol className="list-decimal space-y-1 pl-5">
        <li>Нажмите «Показать код» — он действует 2 минуты.</li>
        <li>В окне расширения Dropship: Настройки → «Код привязки» → «Привязать».</li>
      </ol>
      <div className="flex items-center gap-3">
        <Button kind="primary" onClick={async () => setCode((await run(() => call<string>('start_pairing'))) ?? '')}>
          Показать код
        </Button>
        {code && <span className="rounded-lg bg-slate-100 px-3 py-1.5 font-mono text-xl tracking-widest dark:bg-slate-800">{code}</span>}
        {paired && (
          <Button
            kind="danger"
            onClick={async () => {
              if ((await run(() => call('revoke_extension'), 'Привязка снята')) !== undefined) void load();
            }}
          >
            Отвязать
          </Button>
        )}
      </div>
    </div>
  );
}

function StoresCard() {
  const run = useAction();
  const [stores, setStores] = useState<StoreRow[]>([]);
  const [rulesFor, setRulesFor] = useState<number | null>(null);
  const [form, setForm] = useState({ platform: 'ozon', name: '', clientId: '', apiKey: '', clientSecret: '', warehouse: '' });
  const load = useCallback(async () => {
    const s = await run(() => call<StoreRow[]>('list_stores'));
    if (s) setStores(s);
  }, [run]);
  useEffect(() => {
    void load();
  }, [load]);
  const set = (k: keyof typeof form, v: string) => setForm((f) => ({ ...f, [k]: v }));

  return (
    <Card title="Магазины" hint="Подключите кабинет Ozon или Avito: цены и остатки будут уходить на витрину автоматически.">
      <ul className="mb-4 space-y-2">
        {stores.map((s) => (
          <li key={s.id} className="rounded-lg border border-slate-200 p-3 dark:border-slate-700">
            <div className="flex flex-wrap items-center gap-2">
              <b>{s.name}</b>
              <Pill tone="idle">{s.platform_type === 'local' ? 'локальный каталог' : s.platform_type}</Pill>
              <span className="text-sm text-slate-500">товаров: {s.products}</span>
              {!s.is_active && <Pill tone="warn">выключен</Pill>}
              <Button onClick={() => setRulesFor(rulesFor === s.id ? null : s.id)}>Правила цен</Button>
              {s.platform_type !== 'local' && <Button onClick={async () => { if ((await run(() => call('set_store_active', { storeId: s.id, active: !s.is_active }), 'Готово')) !== undefined) void load(); }}>{s.is_active ? 'Выключить' : 'Включить'}</Button>}
              {s.products === 0 && <Button kind="danger" onClick={async () => { if ((await run(() => call('delete_store', { storeId: s.id }), 'Магазин удалён')) !== undefined) void load(); }}>Удалить</Button>}
            </div>
            {s.last_error && <p className="mt-1 text-sm text-red-700 dark:text-red-400">Последняя ошибка обмена: {s.last_error}</p>}
            {rulesFor === s.id && <RulesEditor storeId={s.id} />}
          </li>
        ))}
      </ul>
      <h3 className="mb-2 font-medium">Подключить магазин</h3>
      <div className="grid gap-x-4 sm:grid-cols-2">
        <Field label="Площадка">
          <select value={form.platform} onChange={(e) => set('platform', e.target.value)} className={inputCls}>
            <option value="ozon">Ozon</option>
            <option value="avito">Avito</option>
          </select>
        </Field>
        <Field label="Название (для вас)">
          <input value={form.name} onChange={(e) => set('name', e.target.value)} className={inputCls} />
        </Field>
        <Field label="Client ID" note={form.platform === 'ozon' ? 'Ozon Seller → Настройки → API ключи' : 'Avito → Профиль → Интеграции → API'}>
          <input value={form.clientId} onChange={(e) => set('clientId', e.target.value)} className={inputCls} />
        </Field>
        {form.platform === 'ozon' ? (
          <>
            <Field label="API-ключ">
              <input type="password" autoComplete="off" value={form.apiKey} onChange={(e) => set('apiKey', e.target.value)} className={inputCls} />
            </Field>
            <Field label="ID склада" note="Номер склада FBS/rFBS из кабинета Ozon">
              <input value={form.warehouse} onChange={(e) => set('warehouse', e.target.value)} className={inputCls} />
            </Field>
          </>
        ) : (
          <Field label="Client secret">
            <input type="password" autoComplete="off" value={form.clientSecret} onChange={(e) => set('clientSecret', e.target.value)} className={inputCls} />
          </Field>
        )}
      </div>
      <Button
        kind="primary"
        onClick={async () => {
          const credentials = form.platform === 'ozon' ? { client_id: form.clientId.trim(), api_key: form.apiKey.trim() } : { client_id: form.clientId.trim(), client_secret: form.clientSecret.trim() };
          const id = await run(() => call<number>('add_store', { input: { name: form.name, platform: form.platform, credentials, warehouse_id: form.warehouse.trim() || null } }), 'Магазин подключён');
          if (id !== undefined) {
            setForm({ platform: form.platform, name: '', clientId: '', apiKey: '', clientSecret: '', warehouse: '' });
            void load();
          }
        }}
      >
        Подключить
      </Button>
      <p className="mt-2 text-xs text-slate-500">Ключи шифруются (AES-256-GCM) и не показываются после сохранения.</p>
    </Card>
  );
}

export function RulesEditor({ storeId }: { storeId: number }) {
  const run = useAction();
  const [rules, setRules] = useState<Rule[]>([]);
  useEffect(() => {
    void run(() => call<Rule[]>('list_rules', { storeId })).then(async (r) => {
      if (r && r.length > 0) setRules(r);
      else setRules(r ? [await call<Rule>('standard_rule')] : []);
    });
  }, [storeId, run]);
  const patch = (i: number, p: Partial<Rule>) => setRules((list) => list.map((r, j) => (i === j ? { ...r, ...p } : r)));
  const num = (v: string) => (v === '' ? 0 : Number(v));
  return (
    <div className="mt-3 space-y-3 border-t border-slate-100 pt-3 dark:border-slate-800">
      {rules.map((r, i) => (
        <div key={i} className="grid gap-x-3 rounded-lg bg-slate-50 p-3 sm:grid-cols-4 dark:bg-slate-800">
          <Field label="Закупка от, ₽">
            <input type="number" min={0} value={r.price_from_minor / 100} onChange={(e) => patch(i, { price_from_minor: toMinor(num(e.target.value)) })} className={inputCls} />
          </Field>
          <Field label="до, ₽">
            <input type="number" min={0} value={Math.min(r.price_to_minor, 1e11) / 100} onChange={(e) => patch(i, { price_to_minor: toMinor(num(e.target.value)) })} className={inputCls} />
          </Field>
          <Field label="Наценка, %">
            <input type="number" min={0} value={r.margin_percent} onChange={(e) => patch(i, { margin_percent: num(e.target.value) })} className={inputCls} />
          </Field>
          <Field label="+ фиксированно, ₽">
            <input type="number" min={0} value={r.fixed_markup_minor / 100} onChange={(e) => patch(i, { fixed_markup_minor: toMinor(num(e.target.value)) })} className={inputCls} />
          </Field>
          <Field label="Комиссия площадки, %">
            <input type="number" min={0} max={90} value={r.marketplace_fee_percent} onChange={(e) => patch(i, { marketplace_fee_percent: num(e.target.value) })} className={inputCls} />
          </Field>
          <Field label="Эквайринг и налог, %">
            <input type="number" min={0} max={90} value={r.acquiring_and_tax_percent} onChange={(e) => patch(i, { acquiring_and_tax_percent: num(e.target.value) })} className={inputCls} />
          </Field>
          <Field label="Зачёркнутая цена: скидка, %">
            <input type="number" min={0} max={90} value={r.discount_striken_percent} onChange={(e) => patch(i, { discount_striken_percent: num(e.target.value) })} className={inputCls} />
          </Field>
          <Field label="Округлять до, ₽">
            <input type="number" min={0} value={r.rounding_minor / 100} onChange={(e) => patch(i, { rounding_minor: toMinor(num(e.target.value)) })} className={inputCls} />
          </Field>
          <Field label="Мин. остаток поставщика" note="Ниже — на витрину отправляем 0">
            <input type="number" min={0} value={r.min_stock_threshold} onChange={(e) => patch(i, { min_stock_threshold: num(e.target.value) })} className={inputCls} />
          </Field>
          <Field label="Мин. прибыль, ₽" note={`Сейчас ${rub(r.min_profit_minor)}`}>
            <input type="number" min={0} value={r.min_profit_minor / 100} onChange={(e) => patch(i, { min_profit_minor: toMinor(num(e.target.value)) })} className={inputCls} />
          </Field>
          {rules.length > 1 && (
            <div className="flex items-end pb-3">
              <Button kind="danger" onClick={() => setRules((list) => list.filter((_, j) => j !== i))}>
                Убрать диапазон
              </Button>
            </div>
          )}
        </div>
      ))}
      <div className="flex flex-wrap gap-2">
        <Button onClick={async () => { const base = await call<Rule>('standard_rule'); const last = rules[rules.length - 1]; setRules([...rules, { ...base, rule_name: 'Диапазон', price_from_minor: last ? last.price_to_minor + 1 : 0 }]); }}>Добавить диапазон цен</Button>
        <Button kind="primary" onClick={async () => { const n = await run(() => call<number>('save_rules', { storeId, rules }), 'Правила сохранены, цены пересчитаны'); if (n !== undefined) return; }}>
          Сохранить и пересчитать цены
        </Button>
      </div>
    </div>
  );
}

function ProxiesCard() {
  const run = useAction();
  const [list, setList] = useState<{ id: number; fail_count: number; cooldown_until: number; is_active: boolean }[]>([]);
  const [url, setUrl] = useState('');
  const load = useCallback(async () => {
    const r = await run(() => call<typeof list>('list_proxies'));
    if (r) setList(r);
  }, [run]);
  useEffect(() => {
    void load();
  }, [load]);
  return (
    <Card title="Прокси для мониторинга (необязательно)" hint="Нужны, если сайт поставщика блокирует частые проверки. Адрес вида http://логин:пароль@хост:порт или socks5://хост:порт.">
      <ul className="mb-3 text-sm">
        {list.map((p) => (
          <li key={p.id} className="flex items-center gap-2 py-1">
            Прокси №{p.id} {p.cooldown_until > Date.now() / 1000 ? <Pill tone="warn">на паузе</Pill> : <Pill tone="good">активен</Pill>} ошибок: {p.fail_count}
            <Button kind="danger" onClick={async () => { if ((await run(() => call('remove_proxy', { proxyId: p.id }))) !== undefined) void load(); }}>Удалить</Button>
          </li>
        ))}
      </ul>
      <div className="flex gap-2">
        <input value={url} onChange={(e) => setUrl(e.target.value)} placeholder="http://user:pass@host:8080" className={inputCls + ' max-w-md'} type="password" autoComplete="off" />
        <Button onClick={async () => { if ((await run(() => call('add_proxy', { proxyUrl: url }), 'Прокси добавлен')) !== undefined) { setUrl(''); void load(); } }}>Добавить</Button>
      </div>
    </Card>
  );
}

function UpdateCard() {
  const run = useAction();
  const [info, setInfo] = useState<UpdateInfo | null>(null);
  return (
    <Card title="Обновление" hint="Приложение само ничего не заменяет: вы скачиваете проверенный установщик со страницы релиза.">
      <Button
        onClick={async () => {
          const r = await run(() => call<UpdateInfo>('check_update'));
          if (r) setInfo(r);
        }}
      >
        Проверить обновления
      </Button>
      {info && (
        <div className="mt-3 text-sm">
          {info.update_available ? (
            <>
              <p>
                Доступна версия <b>{info.latest}</b> (у вас {info.current}).
              </p>
              {info.notes && <pre className="my-2 whitespace-pre-wrap rounded bg-slate-50 p-2 dark:bg-slate-800">{info.notes}</pre>}
              <Button kind="primary" onClick={() => void run(() => call('open_release_page', { url: info.release_url }))}>
                Обновить — открыть страницу загрузки
              </Button>
            </>
          ) : (
            <p>У вас последняя версия ({info.current}). {info.notes}</p>
          )}
        </div>
      )}
    </Card>
  );
}

export function SettingsTab() {
  return (
    <div className="space-y-4">
      <StoresCard />
      <Card title="Искусственный интеллект" hint="Пишет заголовки, описания и характеристики. Можно бесплатно на своём компьютере (Ollama) или через облако.">
        <AiForm />
      </Card>
      <Card title="Расширение браузера">
        <PairingBox />
      </Card>
      <ProxiesCard />
      <UpdateCard />
    </div>
  );
}
