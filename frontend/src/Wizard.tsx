import { useState } from 'react';
import { call, type Onboarding } from './api';
import { AiForm, PairingBox } from './tabs/SettingsTab';
import { Button, Card, useAction } from './ui';

const STEPS = ['Знакомство', 'Каталог и цены', 'ИИ для карточек', 'Расширение', 'Готово'];

export function Wizard({ state, onFinish }: { state: Onboarding; onFinish: () => void }) {
  const run = useAction();
  const [step, setStep] = useState(0);
  const [catalogReady, setCatalogReady] = useState(state.has_store && state.has_rules);

  return (
    <div className="mx-auto max-w-2xl space-y-4 p-6">
      <ol className="flex flex-wrap gap-2 text-xs">
        {STEPS.map((s, i) => (
          <li key={s} className={`rounded-full px-3 py-1 ${i === step ? 'bg-emerald-600 text-white' : i < step ? 'bg-emerald-100 text-emerald-800 dark:bg-emerald-950 dark:text-emerald-300' : 'bg-slate-100 text-slate-500 dark:bg-slate-800'}`}>
            {i + 1}. {s}
          </li>
        ))}
      </ol>

      {step === 0 && (
        <Card title="Добро пожаловать в Dropship">
          <p className="mb-2 text-sm">Программа работает на вашем компьютере: собирает товары поставщиков, считает цену с учётом комиссий, следит за ценами и остатками и подсказывает при заказах.</p>
          <ul className="mb-4 list-disc space-y-1 pl-5 text-sm">
            <li>Настройка займёт пару минут, всё можно изменить позже в «Настройках».</li>
            <li>Ключи хранятся зашифрованными, наружу данные не отправляются (кроме выбранного вами ИИ-сервиса и самих площадок).</li>
            <li>Оплату заказов у поставщиков вы подтверждаете сами — автооплаты нет.</li>
          </ul>
          <Button kind="primary" onClick={() => setStep(1)}>Начать</Button>
        </Card>
      )}

      {step === 1 && (
        <Card title="Каталог и правила цен" hint="Создадим локальный каталог и стандартное правило: наценка 50%, комиссия 15%, налог и эквайринг 6%, минимальная прибыль 300 ₽. Позже поменяете в Настройках.">
          {catalogReady ? <p className="mb-3 text-sm text-emerald-700 dark:text-emerald-400">Каталог уже создан ✓</p> : (
            <Button kind="primary" onClick={async () => { if ((await run(() => call('setup_default_catalog'), 'Каталог создан')) !== undefined) setCatalogReady(true); }}>Создать каталог</Button>
          )}
          <div className="mt-4 flex gap-2">
            <Button onClick={() => setStep(0)}>Назад</Button>
            <Button kind="primary" disabled={!catalogReady} onClick={() => setStep(2)}>Дальше</Button>
          </div>
        </Card>
      )}

      {step === 2 && (
        <Card title="ИИ для карточек" hint="Выберите, кто будет писать заголовки и описания. Можно пропустить и настроить позже — тогда тексты останутся исходными.">
          <AiForm />
          <div className="mt-4 flex gap-2">
            <Button onClick={() => setStep(1)}>Назад</Button>
            <Button kind="primary" onClick={() => setStep(3)}>Дальше</Button>
          </div>
        </Card>
      )}

      {step === 3 && (
        <Card title="Расширение браузера" hint="Через него товары попадают в программу одним кликом. Можно привязать позже: Настройки → Расширение.">
          <PairingBox />
          <div className="mt-4 flex gap-2">
            <Button onClick={() => setStep(2)}>Назад</Button>
            <Button kind="primary" onClick={() => setStep(4)}>Дальше</Button>
          </div>
        </Card>
      )}

      {step === 4 && (
        <Card title="Всё готово">
          <p className="mb-3 text-sm">Закрытие окна сворачивает программу в трей — мониторинг цен продолжает работать. Выйти полностью можно из меню значка в трее.</p>
          <Button kind="primary" onClick={async () => { if ((await run(() => call('finish_onboarding'))) !== undefined) onFinish(); }}>Открыть программу</Button>
        </Card>
      )}
    </div>
  );
}
