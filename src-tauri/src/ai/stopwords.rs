//! Фильтр запрещённых слов: за один проход (aho-corasick) находит стоп-слова площадок
//! (реплики, подделки) в тексте. Проверка идёт по подстроке без учёта регистра.

use aho_corasick::AhoCorasick;

/// Базовый словарь. Пользователь может дополнить его списком в настройках.
pub const DEFAULT_STOP_WORDS: &[&str] = &[
    "реплика", "копия", "подделка", "фейк", "паль", "как оригинал", "1:1", "replica", "fake", "counterfeit",
];

pub struct StopWordFilter {
    automaton: Option<AhoCorasick>,
    words: Vec<String>,
}

impl StopWordFilter {
    pub fn new(extra: &[String]) -> Self {
        let mut words: Vec<String> = DEFAULT_STOP_WORDS
            .iter()
            .map(|w| w.to_string())
            .chain(extra.iter().map(|w| w.trim().to_string()))
            .filter(|w| !w.is_empty())
            .map(|w| w.to_lowercase())
            .collect();
        words.sort();
        words.dedup();
        let automaton = AhoCorasick::new(&words).ok();
        Self { automaton, words }
    }

    /// Найденные стоп-слова без повторов, в порядке словаря.
    pub fn find(&self, text: &str) -> Vec<String> {
        let Some(automaton) = &self.automaton else { return Vec::new() };
        let lowered = text.to_lowercase();
        let mut found: Vec<usize> = automaton.find_overlapping_iter(&lowered).map(|m| m.pattern().as_usize()).collect();
        found.sort_unstable();
        found.dedup();
        found.into_iter().filter_map(|i| self.words.get(i).cloned()).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_words_case_insensitively_without_duplicates() {
        let f = StopWordFilter::new(&[]);
        assert_eq!(f.find("РЕПЛИКА часов, реплика! Люкс копия"), vec!["копия", "реплика"]);
        assert!(f.find("Обычные наушники").is_empty());
        assert!(f.find("").is_empty());
    }

    #[test]
    fn user_words_extend_the_dictionary() {
        let f = StopWordFilter::new(&["  Брендик ".to_string(), "".to_string()]);
        assert_eq!(f.find("футболка БРЕНДИК"), vec!["брендик"]);
    }
}
