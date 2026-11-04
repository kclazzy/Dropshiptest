#pragma once

// Применяет свежие данные поставщика к карточке: сравнивает хэш состояния, считает розничную цену
// через PricingEngine, защищает остатки, обновляет расписание проверки. История цен пишется
// триггерами БД, поэтому здесь её записывать не нужно.

#include <vector>

#include "db/DatabaseManager.hpp"
#include "monitor/PageParser.hpp"

namespace dropship::monitor {

struct ApplyOutcome {
    int unchanged = 0;       // хэш состояния не изменился — запись пропущена
    int changed = 0;         // цена/остаток изменились
    int added = 0;           // поставщик добавил новый вариант
    int soldOut = 0;         // вариант исчез со страницы или остаток ниже порога — публиковать нужно 0
    int unpriced = 0;        // для закупочной цены нет правила — розничная цена не изменена
};

// Свежие данные -> БД. Одна транзакция: либо применено всё, либо ничего.
db::Result<ApplyOutcome> applyObservation(db::DatabaseManager& database, std::int64_t productId,
                                          const std::vector<ObservedVariant>& observed, UnixSeconds now);

// Страница товара пропала (404/410): остатки в 0, мониторинг на паузе.
db::Result<void> markProductUnavailable(db::DatabaseManager& database, std::int64_t productId, UnixSeconds now);

// Расширение передало свежую сессию домена (капча пройдена): карточки этого домена, застрявшие
// в CAPTCHA_BLOCKED, возвращаются в мониторинг. Совпадает сам домен и его поддомены. Возвращает число карточек.
db::Result<int> resumeBlockedProducts(db::DatabaseManager& database, const std::string& domain);

// Ставит в очередь PRICE_SYNC для карточек, чей срок проверки наступил. Идемпотентно:
// для карточки с уже ожидающей/выполняемой задачей новая не создаётся.
db::Result<int> scheduleDueChecks(db::DatabaseManager& database, UnixSeconds now, int limit = 200);

}  // namespace dropship::monitor
