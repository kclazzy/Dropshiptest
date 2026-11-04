#pragma once

#include <string>

#include "ai/Listing.hpp"

namespace dropship::ai {

// Источник текста карточки: локальная модель или облачный провайдер.
// Очередь задач работает только с этим интерфейсом.
class IContentGenerator {
public:
    virtual ~IContentGenerator() = default;
    virtual Result<GeneratedContent> generate(const ProductSource& source) = 0;
    // Для журналов и интерфейса, например "openai:gpt-4o-mini". Никогда не содержит ключ API.
    virtual std::string name() const = 0;
};

}  // namespace dropship::ai
