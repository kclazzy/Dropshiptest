import { useEffect, useState } from 'react';
import { call } from './api';
import { Card, Field, inputCls, rub, toMinor } from './ui';

interface PriceBreakdown {
  supplier_price: number;
  logistics: number;
  total_cost: number;
  marketplace_fee: number;
  acquiring_and_tax: number;
  net_profit: number;
  retail_price: number;
  min_price: number;
  old_price: number;
  raised_to_min_profit: boolean;
}

const PRESETS = [
  { name: 'Осторожный старт +30%', margin: 30 },
  { name: 'Стандартный маркетплейс +50%', margin: 50 },
];

export function Calculator() {
  const [supplier, setSupplier] = useState(1000);
  const [logistics, setLogistics] = useState(150);
  const [margin, setMargin] = useState(50);
  const [fee, setFee] = useState(15);
  const [result, setResult] = useState<PriceBreakdown | null>(null);
  const [error, setError] = useState('');

  useEffect(() => {
    let cancelled = false;
    call<PriceBreakdown>('price_breakdown', {
      request: {
        supplierPriceMinor: toMinor(supplier),
        logisticsMinor: toMinor(logistics),
        packagingMinor: 0,
        marginPercent: margin,
        marketplaceFeePercent: fee,
        acquiringAndTaxPercent: 6,
        discountStrikenPercent: 30,
        roundingMinor: 0,
        minProfitMinor: 0,
      },
    }).then(
      (value) => !cancelled && (setResult(value), setError('')),
      (e: Error) => !cancelled && setError(e.message),
    );
    return () => {
      cancelled = true;
    };
  }, [supplier, logistics, margin, fee]);

  return (
    <Card title="Калькулятор прибыли" hint="Двигайте ползунки — раскладка цены пересчитается сразу.">
      <div className="mb-3 flex flex-wrap gap-2">
        {PRESETS.map((p) => (
          <button key={p.name} onClick={() => setMargin(p.margin)} className="rounded-lg border border-slate-300 px-3 py-1 text-xs hover:bg-slate-50 dark:border-slate-600 dark:hover:bg-slate-800">
            {p.name}
          </button>
        ))}
      </div>
      <div className="grid gap-x-4 sm:grid-cols-2">
        <Field label="Закупка, ₽">
          <input type="number" min={1} value={supplier} onChange={(e) => setSupplier(Number(e.target.value))} className={inputCls} />
        </Field>
        <Field label="Доставка до покупателя, ₽">
          <input type="number" min={0} value={logistics} onChange={(e) => setLogistics(Number(e.target.value))} className={inputCls} />
        </Field>
        <Field label={`Наценка: ${margin}%`}>
          <input type="range" min={0} max={200} value={margin} onChange={(e) => setMargin(Number(e.target.value))} className="w-full" />
        </Field>
        <Field label={`Комиссия площадки: ${fee}%`}>
          <input type="range" min={0} max={40} value={fee} onChange={(e) => setFee(Number(e.target.value))} className="w-full" />
        </Field>
      </div>
      {error && <p className="rounded bg-red-50 p-3 text-sm text-red-700 dark:bg-red-950 dark:text-red-300">{error}</p>}
      {result && !error && (
        <div className="rounded-lg bg-slate-50 p-3 text-sm leading-7 dark:bg-slate-800">
          Закупка {rub(result.supplier_price)} ➔ доставка {rub(result.logistics)} ➔ комиссия и налоги {rub(result.marketplace_fee + result.acquiring_and_tax)} ➔{' '}
          <b>цена на витрине {rub(result.retail_price)}</b>
          <div className="text-lg font-semibold text-emerald-700 dark:text-emerald-400">Ваша чистая прибыль: {rub(result.net_profit)}</div>
          <div className="text-slate-500 dark:text-slate-400">
            Минимальная цена для акций: {rub(result.min_price)} · зачёркнутая цена: {rub(result.old_price)}
          </div>
        </div>
      )}
    </Card>
  );
}
