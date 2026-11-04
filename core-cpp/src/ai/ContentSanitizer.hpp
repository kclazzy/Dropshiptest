#pragma once

#include <string>
#include <string_view>

namespace dropship::ai {

// Оставляет только безопасную разметку описания: p, br, ul, ol, li, b, strong, i, em, h2-h4
// (без атрибутов). Остальные теги снимаются с сохранением текста; script/style/iframe и
// подобные удаляются вместе с содержимым; комментарии удаляются; одиночные '<', '>' и '&'
// экранируются. Описание строится из текста чужих страниц, поэтому ему нельзя доверять.
std::string sanitizeHtml(std::string_view html);

}  // namespace dropship::ai
