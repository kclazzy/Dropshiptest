import { useCallback, useEffect, useState } from 'react';
import { call, type ProductDetail } from './api';
import { Button, Card, Field, inputCls, Pill, rub, useAction, when } from './ui';

const SYNC: Record<string, { tone: 'good' | 'warn' | 'bad' | 'idle'; text: string }> = {
  synced: { tone: 'good', text: 'на витрине актуально' },
  pending: { tone: 'warn', text: 'ждёт отправки' },
  rejected: { tone: 'bad', text: 'площадка отклонила' },
  not_linked: { tone: 'idle', text: 'не привязан' },
};

/** Мини-график розничной цены по истории (SVG, без библиотек). */
function Spark({ points }: { points: number[] }) {
  if (points.length < 2) return <span className="text-xs text-slate-400">мало данных</span>;
  const min = Math.min(...points);
  const span = Math.max(...points) - min || 1;
  const path = points.map((p, i) => `${(i / (points.length - 1)) * 120},${28 - ((p - min) / span) * 24}`).join(' ');
  return (
    <svg width="120" height="32" role="img" aria-label="динамика цены">
      <polyline points={path} fill="none" stroke="currentColor" strokeWidth="2" className="text-emerald-600" />
    </svg>
  );
}

export function ProductPanel({ productId, onChanged, onClose }: { productId: number; onChanged: () => void; onClose: () => void }) {
  const run = useAction();
  const [detail, setDetail] = useState<ProductDetail | null>(null);
  const [title, setTitle] = useState('');
  const [description, setDescription] = useState('');
  const [links, setLinks] = useState<Record<number, string>>({});

  const load = useCallback(async () => {
    const d = await run(() => call<ProductDetail>('product_detail', { productId }));
    if (!d) return;
    setDetail(d);
    setTitle(d.ai_title ?? d.raw_title);
    setDescription(d.ai_description ?? d.raw_description ?? '');
    setLinks(Object.fromEntries(d.variants.map((v) => [v.id, v.external_variant_id ?? ''])));
  }, [productId, run]);

  useEffect(() => {
    void load();
  }, [load]);

  if (!detail) return <Card>Загрузка…</Card>;
  const after = (fn: () => Promise<unknown>, ok: string) => async () => {
    if ((await run(fn, ok)) !== undefined) {
      await load();
      onChanged();
    }
  };

  return (
    <Card className="space-y-4">
      <div className="flex items-start justify-between gap-3">
        <div>
          <h2 className="text-lg font-semibold">Карточка товара №{detail.id}</h2>
          <a className="break-all text-xs text-slate-500 underline" href={detail.supplier_url} target="_blank" rel="noreferrer">
            {detail.supplier_url}
          </a>
        </div>
        <Button onClick={onClose}>Закрыть</Button>
      </div>

      <div className="flex flex-wrap items-center gap-2 text-sm">
        <span>Текст:</span>
        <Pill tone={detail.ai_status === 'READY' ? 'good' : detail.ai_status === 'ERROR' ? 'bad' : 'warn'}>
          {{ READY: 'готов', PENDING: 'в очереди', PROCESSING: 'пишется', ERROR: 'ошибка' }[detail.ai_status] ?? detail.ai_status}
        </Pill>
        <Button onClick={after(() => call('retry_ai', { productId }), 'Поставлено в очередь на генерацию')}>Сгенерировать заново</Button>
        <span className="ml-3">Мониторинг:</span>
        <Pill tone={detail.monitor_status === 'ACTIVE' ? 'good' : 'idle'}>{detail.monitor_status === 'ACTIVE' ? 'включён' : detail.monitor_status === 'ARCHIVED' ? 'товар снят у поставщика' : 'выключен'}</Pill>
        <Button onClick={after(() => call('set_monitoring', { productId, active: detail.monitor_status !== 'ACTIVE' }), 'Готово')}>
          {detail.monitor_status === 'ACTIVE' ? 'Выключить' : 'Включить'}
        </Button>
      </div>

      <div className="grid gap-x-4 md:grid-cols-2">
        <Field label="Название на витрине" note={`Исходное: ${detail.raw_title}`}>
          <input value={title} onChange={(e) => setTitle(e.target.value)} className={inputCls} maxLength={500} />
        </Field>
        <Field label="Описание">
          <textarea value={description} onChange={(e) => setDescription(e.target.value)} rows={4} className={inputCls} />
        </Field>
      </div>
      <Button kind="primary" onClick={after(() => call('update_product_text', { productId, title, description }), 'Текст сохранён')}>
        Сохранить текст
      </Button>

      <div className="overflow-x-auto">
        <table className="w-full text-left text-sm">
          <thead className="text-xs uppercase text-slate-500">
            <tr>
              <th className="py-1 pr-3">Артикул</th>
              <th className="pr-3">Закупка</th>
              <th className="pr-3">Цена на витрине</th>
              <th className="pr-3">Остаток</th>
              <th className="pr-3">Динамика</th>
              <th className="pr-3">ID на площадке</th>
              <th>Статус</th>
            </tr>
          </thead>
          <tbody>
            {detail.variants.map((v) => {
              const prices = detail.history.filter((h) => h.variant_id === v.id).map((h) => h.retail_price_minor).reverse();
              const s = SYNC[v.sync_state] ?? { tone: 'idle' as const, text: v.sync_state };
              return (
                <tr key={v.id} className="border-t border-slate-100 align-top dark:border-slate-800">
                  <td className="py-2 pr-3">{v.supplier_sku}</td>
                  <td className="pr-3">{rub(v.supplier_price_minor)}</td>
                  <td className="pr-3">{v.retail_price_minor > 0 ? rub(v.retail_price_minor) : <span className="text-slate-400">нет правила</span>}</td>
                  <td className="pr-3">{v.is_in_stock ? v.supplier_stock : <Pill tone="bad">нет</Pill>}</td>
                  <td className="pr-3">
                    <Spark points={prices} />
                  </td>
                  <td className="pr-3">
                    <div className="flex gap-1">
                      <input value={links[v.id] ?? ''} onChange={(e) => setLinks({ ...links, [v.id]: e.target.value })} placeholder="offer_id / номер объявления" className={inputCls + ' !w-44'} />
                      <Button onClick={after(() => call('link_variant', { variantId: v.id, externalId: links[v.id] ?? '' }), 'Привязка сохранена')}>OK</Button>
                    </div>
                  </td>
                  <td>
                    <Pill tone={s.tone}>{s.text}</Pill>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>

      <details>
        <summary className="cursor-pointer text-sm font-medium">История цен и остатков ({detail.history.length})</summary>
        <table className="mt-2 w-full text-left text-sm">
          <thead className="text-xs uppercase text-slate-500">
            <tr>
              <th>Когда</th>
              <th>Артикул</th>
              <th>Закупка</th>
              <th>Витрина</th>
              <th>Остаток</th>
            </tr>
          </thead>
          <tbody>
            {detail.history.map((h, i) => (
              <tr key={i} className="border-t border-slate-100 dark:border-slate-800">
                <td className="py-1">{when(h.recorded_at)}</td>
                <td>{detail.variants.find((v) => v.id === h.variant_id)?.supplier_sku}</td>
                <td>{rub(h.supplier_price_minor)}</td>
                <td>{h.retail_price_minor > 0 ? rub(h.retail_price_minor) : '—'}</td>
                <td>{h.supplier_stock}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </details>

      <Button
        kind="danger"
        onClick={async () => {
          if (!window.confirm('Удалить товар вместе с историей цен? Заказы сохранятся.')) return;
          if ((await run(() => call('delete_product', { productId }), 'Товар удалён')) !== undefined) {
            onChanged();
            onClose();
          }
        }}
      >
        Удалить товар
      </Button>
    </Card>
  );
}
