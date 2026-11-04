import { useEffect, useState } from 'react';
import { invoke } from '@tauri-apps/api/core';
import { rub, toMinor } from './money';

interface PriceBreakdown {
  supplier_price: number;
  logistics: number;
  packaging: number;
  total_cost: number;
  marketplace_fee: number;
  acquiring_and_tax: number;
  net_profit: number;
  retail_price: number;
  min_price: number;
  old_price: number;
  profit_margin_bp: number;
  raised_to_min_profit: boolean;
}

const PRESETS = [
  { name: 'Осторожный старт +30%', margin: 30 },
  { name: 'Стандартный маркетплейс +50%', margin: 50 },
];

export function App() {
  const [supplier, setSupplier] = useState(1000);
  const [logistics, setLogistics] = useState(150);
  const [margin, setMargin] = useState(50);
  const [fee, setFee] = useState(15);
  const [result, setResult] = useState<PriceBreakdown | null>(null);
  const [error, setError] = useState('');

  useEffect(() => {
    let cancelled = false;
    invoke<PriceBreakdown>('price_breakdown', {
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
      (value) => {
        if (cancelled) return;
        setResult(value);
        setError('');
      },
      (e) => !cancelled && setError(String(e)),
    );
    return () => {
      cancelled = true;
    };
  }, [supplier, logistics, margin, fee]);

  return (
    <main className="mx-auto max-w-xl p-6 font-sans text-slate-800">
      <h1 className="mb-1 text-2xl font-semibold">Калькулятор прибыли</h1>
      <p className="mb-6 text-sm text-slate-500">Двигайте ползунок — раскладка цены пересчитается сразу.</p>

      <div className="mb-4 flex gap-2">
        {PRESETS.map((p) => (
          <button key={p.name} onClick={() => setMargin(p.margin)} className="rounded-lg border px-3 py-1.5 text-sm hover:bg-slate-50">
            {p.name}
          </button>
        ))}
      </div>

      <label className="mb-3 block text-sm">
        Закупка, ₽
        <input type="number" min={1} value={supplier} onChange={(e) => setSupplier(Number(e.target.value))} className="mt-1 w-full rounded border p-2" />
      </label>
      <label className="mb-3 block text-sm">
        Доставка до покупателя, ₽
        <input type="number" min={0} value={logistics} onChange={(e) => setLogistics(Number(e.target.value))} className="mt-1 w-full rounded border p-2" />
      </label>
      <label className="mb-3 block text-sm">
        Наценка: {margin}%
        <input type="range" min={0} max={200} value={margin} onChange={(e) => setMargin(Number(e.target.value))} className="w-full" />
      </label>
      <label className="mb-6 block text-sm">
        Комиссия площадки: {fee}%
        <input type="range" min={0} max={40} value={fee} onChange={(e) => setFee(Number(e.target.value))} className="w-full" />
      </label>

      {error && <p className="rounded bg-red-50 p-3 text-sm text-red-700">{error}</p>}
      {result && !error && (
        <section className="rounded-xl bg-slate-50 p-4 text-sm leading-7">
          Закупка {rub(result.supplier_price)} ➔ доставка {rub(result.logistics)} ➔ комиссия и налоги{' '}
          {rub(result.marketplace_fee + result.acquiring_and_tax)} ➔ <b>цена на витрине {rub(result.retail_price)}</b>
          <div className="text-lg font-semibold text-emerald-700">Ваша чистая прибыль: {rub(result.net_profit)}</div>
          <div className="text-slate-500">
            Минимальная цена для акций: {rub(result.min_price)} · зачёркнутая цена: {rub(result.old_price)}
          </div>
        </section>
      )}
    </main>
  );
}
