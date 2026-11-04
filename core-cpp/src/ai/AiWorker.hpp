#pragma once

// Конвейер «товар -> текст карточки»: берёт задачи AI_REWRITE из task_queue,
// вызывает выбранного провайдера (локального или облачного) и сохраняет результат.
// Источник работы один — очередь; products.ai_status только отражает итог.

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "ai/ContentGenerator.hpp"
#include "db/DatabaseManager.hpp"

namespace dropship::ai {

enum class AiTaskOutcome {
    NoTask,    // очередь пуста
    Completed, // карточка сохранена
    Retrying,  // временная ошибка, задача вернётся в очередь позже
    Failed,    // окончательная ошибка (ai_status = ERROR)
};

// Обрабатывает не более одной задачи. Ошибка базы данных возвращается как ошибка;
// ошибки провайдера — это обычные исходы (Retrying/Failed), а не сбой.
db::Result<AiTaskOutcome> processNextAiTask(db::DatabaseManager& database, IContentGenerator& generator, UnixSeconds now,
                                            std::chrono::seconds retryDelay = std::chrono::seconds(60));

using LogFn = std::function<void(const std::string&)>;

// Фоновый поток (std::jthread): обрабатывает очередь, пока не получит stop_token.
// Деструктор ждёт завершения текущей генерации (не дольше таймаута провайдера).
class AiWorker {
public:
    AiWorker(db::DatabaseManager& database, IContentGenerator& generator, std::function<UnixSeconds()> clock,
             LogFn log = {});
    ~AiWorker();
    AiWorker(const AiWorker&) = delete;
    AiWorker& operator=(const AiWorker&) = delete;

private:
    std::jthread thread_;
};

}  // namespace dropship::ai
