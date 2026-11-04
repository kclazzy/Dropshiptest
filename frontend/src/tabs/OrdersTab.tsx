import { useCallback, useEffect, useState } from 'react';
import { call, type OrderRow, type ProductRow, type StoreRow } from '../api';
import { Button, Card, Field, inputCls, Pill, rub, toMinor, useAction } from '../ui';

const STATUS: Record<string, { tone: 'good' | 'warn' | 'bad' | 'idle'; text: string }> = {
  NEW: { tone: 'idle', text: 'новый' },
  READY_TO_BUY: { tone: 'good', text: 'готов к выкупу' },
  QUEUED_FOR_EXT: { tone: 'warn', text: 'нужно выкупить у поставщика' },
  ORDERED: { tone: 'good', text: 'выкуплен, ждём трек' },
  SHIPPED: { tone: 'good', text: 'отправлен' },
  MANUAL_REVIEW: { tone: 'warn', text: 'ждёт вашего решения' },
  ERROR: { tone: 'bad', text: 'выкуп не удался' },
  CANCELLED: { tone: 'idle', text: 'отменён' },
};

function ManualOrderForm({ onDone }: { onDone: () => void }) {
  const run = useAction();
  const [stores, setStores] = useState<StoreRow[]>([]);
  const [products, setProducts] = useState<ProductRow[]>([]);
  const [variants, setVariants] = useState<{ id: number; label: string }[]>([]);
  const [f, setF] = useState({ storeId: 0, productId: 0, variantId: 0, orderId: '', quantity: 1, total: 0, name: '', phone: '', city: '', street: '' });

  useEffect(() => {
    void (async () => {
      const [s, p] = await Promise.all([run(() => call<StoreRow[]>('list_stores')), run(() => call<ProductRow[]>('list_products', { filter: 'all', search: '', limit: 200, offset: 0 }))]);
      if (s) {
        setStores(s);
        setF((x) => ({ ...x, storeId: s[0]?.id ?? 0 }));
      }
      if (p) setProducts(p);
    })();
  }, [run]);

  useEffect(() => {
    if (!f.productId) return setVariants([]);
    void run(() => call<{ variants: { id: number; supplier_sku: string }[] }>('product_detail', { productId: f.productId })).then((d) => {
      const list = (d?.variants ?? []).map((v) => ({ id: v.id, label: v.supplier_sku }));
      setVariants(list);
      setF((x) => ({ ...x, variantId: list[0]?.id ?? 0 }));
    });
  }, [f.productId, run]);

  const set = <K extends keyof typeof f>(k: K, v: (typeof f)[K]) => setF((x) => ({ ...x, [k]: v }));
  return (
    <Card title="Добавить заказ вручную" hint="Пока заказы не скачиваются с площадок автоматически: впишите заказ из личного кабинета Ozon/Avito.">
      <div className="grid gap-x-4 sm:grid-cols-3">
        <Field label="Магазин">
          <select value={f.storeId} onChange={(e) => set('storeId', Number(e.target.value))} className={inputCls}>
            {stores.map((s) => (
              <option key={s.id} value={s.id}>
                {s.name}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Товар">
          <select value={f.productId} onChange={(e) => set('productId', Number(e.target.value))} className={inputCls}>
            <option value={0}>— выберите —</option>
            {products.map((p) => (
              <option key={p.id} value={p.id}>
                {p.title.slice(0, 60)}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Вариант (артикул)">
          <select value={f.variantId} onChange={(e) => set('variantId', Number(e.target.value))} className={inputCls}>
            {variants.map((v) => (
              <option key={v.id} value={v.id}>
                {v.label}
              </option>
            ))}
          </select>
        </Field>
        <Field label="Номер заказа на площадке">
          <input value={f.orderId} onChange={(e) => set('orderId', e.target.value)} className={inputCls} />
        </Field>
        <Field label="Количество">
          <input type="number" min={1} value={f.quantity} onChange={(e) => set('quantity', Number(e.target.value))} className={inputCls} />
        </Field>
        <Field label="Сумма заказа, ₽">
          <input type="number" min={1} value={f.total} onChange={(e) => set('total', Number(e.target.value))} className={inputCls} />
        </Field>
        <Field label="Получатель">
          <input value={f.name} onChange={(e) => set('name', e.target.value)} className={inputCls} />
        </Field>
        <Field label="Телефон">
          <input value={f.phone} onChange={(e) => set('phone', e.target.value)} className={inputCls} />
        </Field>
        <Field label="Город">
          <input value={f.city} onChange={(e) => set('city', e.target.value)} className={inputCls} />
        </Field>
        <Field label="Адрес">
          <input value={f.street} onChange={(e) => set('street', e.target.value)} className={inputCls} />
        </Field>
      </div>
      <Button
        kind="primary"
        disabled={!f.storeId || !f.variantId || !f.orderId.trim() || f.total <= 0}
        onClick={async () => {
          const r = await run(() =>
            call('add_manual_order', {
              order: { store_id: f.storeId, variant_id: f.variantId, external_order_id: f.orderId, quantity: f.quantity, total_minor: toMinor(f.total), buyer: { name: f.name, phone: f.phone, city: f.city, street: f.street } },
            }),
          );
          if (r !== undefined) {
            setF((x) => ({ ...x, orderId: '', total: 0 }));
            onDone();
          }
        }}
      >
        Добавить и проверить
      </Button>
    </Card>
  );
}

function OrderCard({ o, onChanged }: { o: OrderRow; onChanged: () => void }) {
  const run = useAction();
  const [supplierId, setSupplierId] = useState('');
  const [track, setTrack] = useState('');
  const act = (fn: () => Promise<unknown>, ok: string) => async () => {
    if ((await run(fn, ok)) !== undefined) onChanged();
  };
  const st = STATUS[o.status] ?? { tone: 'idle' as const, text: o.status };
  return (
    <li className="rounded-lg border border-slate-200 p-3 dark:border-slate-700">
      <div className="flex flex-wrap items-center gap-2">
        <b>№{o.external_order_id}</b>
        <Pill tone={st.tone}>{st.text}</Pill>
        <span className="text-sm text-slate-500">
          {o.title ?? 'товар не найден'} × {o.quantity} · {rub(o.retail_total_cents)}
          {o.net_profit_cents !== null && <> · прибыль {rub(o.net_profit_cents)}</>}
        </span>
      </div>
      {o.human_readable_hint && <p className="mt-1 text-sm">{o.human_readable_hint}</p>}
      {o.error_message && <p className="mt-1 text-sm text-red-700 dark:text-red-400">{o.error_message}</p>}
      {o.supplier_url && ['QUEUED_FOR_EXT', 'MANUAL_REVIEW', 'ERROR'].includes(o.status) && (
        <p className="mt-1 text-sm">
          Страница поставщика:{' '}
          <a href={o.supplier_url} target="_blank" rel="noreferrer" className="underline">
            открыть
          </a>{' '}
          (артикул {o.sku}, {o.quantity} шт.). Адрес покупателя: {Object.values(o.address).filter(Boolean).join(', ') || '—'}
        </p>
      )}
      <div className="mt-2 flex flex-wrap items-center gap-2">
        {['MANUAL_REVIEW', 'ERROR'].includes(o.status) && <Button onClick={act(() => call('order_action', { orderId: o.id, action: 'retry' }), 'Заказ отправлен на выкуп')}>Выкупить всё равно / повторить</Button>}
        {['NEW', 'READY_TO_BUY', 'MANUAL_REVIEW', 'ERROR'].includes(o.status) && (
          <Button kind="danger" onClick={act(() => call('order_action', { orderId: o.id, action: 'cancel' }), 'Заказ отменён')}>
            Отменить
          </Button>
        )}
        {['QUEUED_FOR_EXT', 'MANUAL_REVIEW', 'ERROR'].includes(o.status) && (
          <>
            <input value={supplierId} onChange={(e) => setSupplierId(e.target.value)} placeholder="№ заказа у поставщика" className={inputCls + ' !w-52'} />
            <Button onClick={act(() => call('order_mark_ordered', { orderId: o.id, supplierOrderId: supplierId }), 'Отмечено: выкуплен')}>Я выкупил</Button>
          </>
        )}
        {o.status === 'ORDERED' && (
          <>
            <input value={track} onChange={(e) => setTrack(e.target.value)} placeholder="Трек-номер" className={inputCls + ' !w-52'} />
            <Button onClick={act(() => call('order_set_tracking', { orderId: o.id, trackingNumber: track, carrier: null }), 'Трек сохранён')}>Сохранить трек</Button>
          </>
        )}
        {o.tracking_number && <span className="text-sm">Трек: {o.tracking_number}</span>}
      </div>
    </li>
  );
}

export function OrdersTab() {
  const run = useAction();
  const [orders, setOrders] = useState<OrderRow[]>([]);
  const [loaded, setLoaded] = useState(false);
  const load = useCallback(async () => {
    const r = await run(() => call<OrderRow[]>('list_orders'));
    if (r) setOrders(r);
    setLoaded(true);
  }, [run]);
  useEffect(() => {
    void load();
    const t = setInterval(() => void load(), 10000);
    return () => clearInterval(t);
  }, [load]);

  return (
    <div className="space-y-4">
      <Card title="Заказы" hint="Режим «ассистированный»: приложение проверяет прибыль и наличие и открывает страницу поставщика, но оплачиваете заказ вы сами.">
        {loaded && orders.length === 0 && <p className="py-6 text-center text-sm text-slate-500">Заказов пока нет.</p>}
        <ul className="space-y-2">
          {orders.map((o) => (
            <OrderCard key={o.id} o={o} onChanged={() => void load()} />
          ))}
        </ul>
      </Card>
      <ManualOrderForm onDone={() => void load()} />
    </div>
  );
}
