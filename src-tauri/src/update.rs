//! Проверка обновлений по релизам GitHub. «Обновить в один клик» открывает страницу релиза с готовым установщиком:
//! тихая замена приложения без подписи релизов (Tauri updater + ключ) была бы небезопасной.

use std::time::Duration;

use serde::{Deserialize, Serialize};

use crate::error::{AppError, AppResult};

pub const GITHUB_API: &str = "https://api.github.com";
pub const REPO: &str = "kclazzy/Dropshiptest";

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct UpdateInfo {
    pub current: String,
    pub latest: String,
    pub update_available: bool,
    pub release_url: String,
    pub notes: String,
}

#[derive(Deserialize)]
struct Release {
    tag_name: String,
    html_url: String,
    #[serde(default)]
    body: Option<String>,
}

/// «v1.2.3» / «1.2.3-beta» → (1, 2, 3). Непонятная версия — `None`, а не «ноль».
pub fn parse_version(text: &str) -> Option<(u64, u64, u64)> {
    let core = text.trim().trim_start_matches(['v', 'V']).split(['-', '+']).next()?;
    let mut parts = core.split('.');
    let triple = (parts.next()?.parse().ok()?, parts.next().unwrap_or("0").parse().ok()?, parts.next().unwrap_or("0").parse().ok()?);
    parts.next().is_none().then_some(triple)
}

pub async fn check(api_base: &str, current: &str) -> AppResult<UpdateInfo> {
    let current_version = parse_version(current).ok_or_else(|| AppError::Validation("не удалось определить текущую версию".into()))?;
    let client = reqwest::Client::builder().timeout(Duration::from_secs(15)).user_agent("dropship-update-check").build().map_err(|e| AppError::Validation(e.to_string()))?;
    let response = client
        .get(format!("{}/repos/{REPO}/releases/latest", api_base.trim_end_matches('/')))
        .header("accept", "application/vnd.github+json")
        .send()
        .await
        .map_err(|_| AppError::Validation("не удалось проверить обновления: нет связи с GitHub".into()))?;
    if response.status().as_u16() == 404 {
        return Ok(UpdateInfo { current: current.into(), latest: current.into(), update_available: false, release_url: format!("https://github.com/{REPO}/releases"), notes: "Релизов пока нет.".into() });
    }
    if !response.status().is_success() {
        return Err(AppError::Validation(format!("GitHub ответил ошибкой {}", response.status().as_u16())));
    }
    let release: Release = response.json().await.map_err(|_| AppError::Validation("GitHub вернул непонятный ответ".into()))?;
    let latest = parse_version(&release.tag_name);
    let release_url = if release.html_url.starts_with("https://github.com/") { release.html_url } else { format!("https://github.com/{REPO}/releases") };
    Ok(UpdateInfo {
        current: current.into(),
        latest: release.tag_name,
        update_available: latest.is_some_and(|l| l > current_version),
        release_url,
        notes: release.body.unwrap_or_default().chars().take(2_000).collect(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::MockServer;

    #[test]
    fn versions_parse_and_compare_numerically() {
        assert_eq!(parse_version("v1.2.3"), Some((1, 2, 3)));
        assert_eq!(parse_version("0.10"), Some((0, 10, 0)));
        assert_eq!(parse_version("2.0.0-beta.1"), Some((2, 0, 0)));
        assert!(parse_version("0.10.0") > parse_version("0.9.9"), "10 > 9, а не строковое сравнение");
        for bad in ["", "abc", "1.2.3.4", "1.x"] {
            assert_eq!(parse_version(bad), None, "{bad}");
        }
    }

    #[tokio::test]
    async fn newer_release_is_reported_with_link() {
        let server = MockServer::start().await;
        server.reply(&format!("/repos/{REPO}/releases/latest"), 200, r#"{"tag_name":"v0.2.0","html_url":"https://github.com/kclazzy/Dropshiptest/releases/tag/v0.2.0","body":"Новое"}"#);
        let info = check(&server.base_url, "0.1.0").await.unwrap();
        assert_eq!((info.update_available, info.latest.as_str(), info.notes.as_str()), (true, "v0.2.0", "Новое"));
        let same = check(&server.base_url, "0.2.0").await.unwrap();
        assert!(!same.update_available);
    }

    #[tokio::test]
    async fn no_releases_and_errors_are_friendly_and_foreign_links_are_replaced() {
        let server = MockServer::start().await;
        let path = format!("/repos/{REPO}/releases/latest");
        server.reply(&path, 404, "{}");
        let none = check(&server.base_url, "0.1.0").await.unwrap();
        assert!(!none.update_available && none.notes.contains("нет"));

        let evil = MockServer::start().await;
        evil.reply(&path, 200, r#"{"tag_name":"v9.0.0","html_url":"https://evil.example/download.exe"}"#);
        assert!(check(&evil.base_url, "0.1.0").await.unwrap().release_url.starts_with("https://github.com/"), "чужая ссылка не показывается");

        let broken = MockServer::start().await;
        broken.reply(&path, 500, "oops");
        assert!(check(&broken.base_url, "0.1.0").await.unwrap_err().to_user_friendly_ru().contains("500"));
        assert!(check("http://127.0.0.1:1", "0.1.0").await.is_err());
        assert!(check(&server.base_url, "garbage").await.is_err());
    }
}
