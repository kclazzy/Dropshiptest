import { useCallback, useEffect, useState } from 'react';
import { call, type Onboarding } from './api';
import { OrdersTab } from './tabs/OrdersTab';
import { ProductsTab } from './tabs/ProductsTab';
import { SettingsTab } from './tabs/SettingsTab';
import { ToastProvider } from './ui';
import { Wizard } from './Wizard';

const TABS = [
  { id: 'drafts', label: '📦 Импорт и Черновики' },
  { id: 'mine', label: '🟢 Мои товары' },
  { id: 'orders', label: '🛍️ Заказы и Сообщения' },
  { id: 'settings', label: '⚙️ Настройки' },
] as const;
type TabId = (typeof TABS)[number]['id'];

export function App() {
  const [tab, setTab] = useState<TabId>('drafts');
  const [onboarding, setOnboarding] = useState<Onboarding | null>(null);
  const [fatal, setFatal] = useState('');

  const load = useCallback(() => {
    call<Onboarding>('onboarding_state').then(setOnboarding, (e: Error) => setFatal(e.message));
  }, []);
  useEffect(load, [load]);

  return (
    <ToastProvider>
      <div className="min-h-screen bg-slate-50 text-slate-800 dark:bg-slate-950 dark:text-slate-100">
        {fatal && <p className="m-6 rounded-lg bg-red-50 p-4 text-sm text-red-700">Не удалось запустить программу: {fatal}</p>}
        {!fatal && !onboarding && <p className="p-6 text-sm text-slate-500">Загрузка…</p>}
        {onboarding && !onboarding.done && <Wizard state={onboarding} onFinish={load} />}
        {onboarding?.done && (
          <>
            <nav className="sticky top-0 z-10 flex gap-1 overflow-x-auto border-b border-slate-200 bg-white px-4 dark:border-slate-800 dark:bg-slate-900" role="tablist">
              {TABS.map((t) => (
                <button
                  key={t.id}
                  role="tab"
                  aria-selected={tab === t.id}
                  onClick={() => setTab(t.id)}
                  className={`whitespace-nowrap border-b-2 px-4 py-3 text-sm font-medium ${tab === t.id ? 'border-emerald-600 text-emerald-700 dark:text-emerald-400' : 'border-transparent text-slate-500 hover:text-slate-800 dark:hover:text-slate-200'}`}
                >
                  {t.label}
                </button>
              ))}
            </nav>
            <main className="mx-auto max-w-5xl p-4">
              {tab === 'drafts' && <ProductsTab mode="drafts" />}
              {tab === 'mine' && <ProductsTab mode="mine" />}
              {tab === 'orders' && <OrdersTab />}
              {tab === 'settings' && <SettingsTab />}
            </main>
          </>
        )}
      </div>
    </ToastProvider>
  );
}
