// Небольшой набор общих элементов: единый вид кнопок, карточек, подсказок.
import { createContext, useCallback, useContext, useState, type ReactNode } from 'react';
import { errorText } from './api';

export const rub = (minor: number): string => `${(minor / 100).toLocaleString('ru-RU', { minimumFractionDigits: 2, maximumFractionDigits: 2 })} ₽`;
export const toMinor = (rubles: number): number => Math.round(rubles * 100);
export const when = (unix: number): string => (unix > 0 ? new Date(unix * 1000).toLocaleString('ru-RU', { dateStyle: 'short', timeStyle: 'short' }) : '—');

const cls = (...parts: (string | false | undefined)[]) => parts.filter(Boolean).join(' ');

export function Button({ kind = 'plain', className, ...rest }: React.ButtonHTMLAttributes<HTMLButtonElement> & { kind?: 'primary' | 'plain' | 'danger' }) {
  const look = {
    primary: 'bg-emerald-600 text-white hover:bg-emerald-700 border-transparent',
    plain: 'bg-white hover:bg-slate-50 border-slate-300 dark:bg-slate-800 dark:border-slate-600 dark:hover:bg-slate-700',
    danger: 'bg-white text-red-700 hover:bg-red-50 border-red-300 dark:bg-slate-800 dark:hover:bg-red-950',
  }[kind];
  return <button {...rest} className={cls('rounded-lg border px-3 py-1.5 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50', look, className)} />;
}

export function Card({ title, hint, children, className }: { title?: string; hint?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={cls('rounded-xl border border-slate-200 bg-white p-4 shadow-sm dark:border-slate-700 dark:bg-slate-900', className)}>
      {title && <h2 className="text-base font-semibold">{title}</h2>}
      {hint && <p className="mb-3 mt-1 text-sm text-slate-500 dark:text-slate-400">{hint}</p>}
      {children}
    </section>
  );
}

export function Field({ label, children, note }: { label: string; children: ReactNode; note?: string }) {
  return (
    <label className="mb-3 block text-sm">
      <span className="mb-1 block font-medium">{label}</span>
      {children}
      {note && <span className="mt-1 block text-xs text-slate-500 dark:text-slate-400">{note}</span>}
    </label>
  );
}

export const inputCls = 'w-full rounded-lg border border-slate-300 bg-white px-3 py-2 text-sm dark:border-slate-600 dark:bg-slate-800';

const PILL: Record<string, string> = {
  good: 'bg-emerald-100 text-emerald-800 dark:bg-emerald-950 dark:text-emerald-300',
  warn: 'bg-amber-100 text-amber-800 dark:bg-amber-950 dark:text-amber-300',
  bad: 'bg-red-100 text-red-800 dark:bg-red-950 dark:text-red-300',
  idle: 'bg-slate-100 text-slate-700 dark:bg-slate-800 dark:text-slate-300',
};
export const Pill = ({ tone, children }: { tone: keyof typeof PILL; children: ReactNode }) => (
  <span className={cls('inline-block rounded-full px-2 py-0.5 text-xs font-medium', PILL[tone])}>{children}</span>
);

// ---- уведомления ----
type Toast = { id: number; text: string; bad: boolean };
const ToastCtx = createContext<(text: string, bad?: boolean) => void>(() => {});
export const useToast = () => useContext(ToastCtx);

export function ToastProvider({ children }: { children: ReactNode }) {
  const [items, setItems] = useState<Toast[]>([]);
  const push = useCallback((text: string, bad = false) => {
    const id = Date.now() + Math.random();
    setItems((list) => [...list, { id, text, bad }]);
    setTimeout(() => setItems((list) => list.filter((t) => t.id !== id)), bad ? 9000 : 4000);
  }, []);
  return (
    <ToastCtx.Provider value={push}>
      {children}
      <div className="fixed bottom-4 right-4 z-50 flex max-w-sm flex-col gap-2" role="status" aria-live="polite">
        {items.map((t) => (
          <div key={t.id} className={cls('rounded-lg px-4 py-3 text-sm shadow-lg', t.bad ? 'bg-red-600 text-white' : 'bg-slate-900 text-white')}>
            {t.text}
          </div>
        ))}
      </div>
    </ToastCtx.Provider>
  );
}

/** Выполняет действие и показывает результат: успех — коротко, ошибка — понятным текстом. */
export function useAction() {
  const toast = useToast();
  return useCallback(
    async <T,>(action: () => Promise<T>, okText?: string): Promise<T | undefined> => {
      try {
        const result = await action();
        if (okText) toast(okText);
        return result;
      } catch (e) {
        toast(errorText(e), true);
        return undefined;
      }
    },
    [toast],
  );
}
