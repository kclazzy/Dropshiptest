import { useCallback, useEffect, useState } from 'react';
import { call, type ProductRow } from '../api';
import { Calculator } from '../Calculator';
import { ProductPanel } from '../ProductPanel';
import { Card, Pill, rub, useAction, when, inputCls } from '../ui';

const AI: Record<string, { tone: 'good' | 'warn' | 'bad'; text: string }> = {
  READY: { tone: 'good', text: 'текст готов' },
  PENDING: { tone: 'warn', text: 'в очереди' },
  PROCESSING: { tone: 'warn', text: 'пишется' },
  ERROR: { tone: 'bad', text: 'ошибка ИИ' },
};

/** Общий список для вкладок «Черновики» (drafts) и «Мои товары» (mine). */
export function ProductsTab({ mode }: { mode: 'drafts' | 'mine' }) {
  const run = useAction();
  const [rows, setRows] = useState<ProductRow[]>([]);
  const [search, setSearch] = useState('');
  const [open, setOpen] = useState<number | null>(null);
  const [loaded, setLoaded] = useState(false);

  const load = useCallback(async () => {
    const r = await run(() => call<ProductRow[]>('list_products', { filter: mode, search, limit: 100, offset: 0 }));
    if (r) setRows(r);
    setLoaded(true);
  }, [mode, search, run]);

  useEffect(() => {
    void load();
    const timer = setInterval(() => void load(), 15000); // подхватываем новые товары из расширения
    return () => clearInterval(timer);
  }, [load]);

  return (
    <div className="space-y-4">
      {mode === 'drafts' && (
        <>
          <Card title="Как добавить товар" hint="Откройте страницу товара у поставщика и нажмите значок расширения Dropship → «Добавить товар с этой страницы».">
            <ol className="list-decimal space-y-1 pl-5 text-sm">
              <li>Установите расширение (папка <code>extension-mv3/dist</code>, см. инструкцию) и привяжите его: Настройки → Расширение.</li>
              <li>Товар появится здесь через несколько секунд; ИИ напишет заголовок и описание.</li>
              <li>Проверьте текст, укажите ID товара на площадке (offer_id Ozon / номер объявления Avito) — после этого цены и остатки пойдут на витрину, а товар переедет во вкладку «Мои товары».</li>
            </ol>
          </Card>
          <Calculator />
        </>
      )}

      <Card title={mode === 'drafts' ? 'Черновики' : 'Мои товары'} hint={mode === 'drafts' ? 'Товары, которые ещё не привязаны к площадке.' : 'Привязанные товары: цены и остатки обновляются автоматически.'}>
        <input value={search} onChange={(e) => setSearch(e.target.value)} placeholder="Поиск по названию…" className={inputCls + ' mb-3 max-w-sm'} />
        {loaded && rows.length === 0 && <p className="py-6 text-center text-sm text-slate-500">{search ? 'Ничего не найдено.' : mode === 'drafts' ? 'Пока пусто — добавьте первый товар через расширение.' : 'Пока нет привязанных товаров.'}</p>}
        <ul className="divide-y divide-slate-100 dark:divide-slate-800">
          {rows.map((p) => (
            <li key={p.id}>
              <button onClick={() => setOpen(open === p.id ? null : p.id)} className="flex w-full items-center gap-3 py-2 text-left hover:bg-slate-50 dark:hover:bg-slate-800">
                {p.image ? <img src={p.image} alt="" className="h-12 w-12 rounded object-cover" loading="lazy" referrerPolicy="no-referrer" /> : <div className="h-12 w-12 rounded bg-slate-200 dark:bg-slate-700" />}
                <div className="min-w-0 flex-1">
                  <div className="truncate font-medium">{p.title}</div>
                  <div className="text-xs text-slate-500">
                    Вариантов: {p.variants} · остаток: {p.stock} · проверено: {when(p.last_checked_at)}
                  </div>
                </div>
                <div className="text-right text-sm">{p.min_retail_minor ? <>от {rub(p.min_retail_minor)}</> : <span className="text-slate-400">цена не рассчитана</span>}</div>
                <Pill tone={AI[p.ai_status]?.tone ?? 'idle'}>{AI[p.ai_status]?.text ?? p.ai_status}</Pill>
                {p.monitor_status !== 'ACTIVE' && <Pill tone="idle">мониторинг выкл.</Pill>}
              </button>
              {open === p.id && <ProductPanel productId={p.id} onChanged={() => void load()} onClose={() => setOpen(null)} />}
            </li>
          ))}
        </ul>
      </Card>
    </div>
  );
}
