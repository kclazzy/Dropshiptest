//! Загрузка страниц поставщика: защита от SSRF, пауза между запросами к одному сайту, пул прокси.

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::time::Duration;

use async_trait::async_trait;
use dashmap::DashMap;
use sqlx::SqlitePool;

use crate::error::{AppError, AppResult};

const MAX_BODY_BYTES: usize = 2 * 1024 * 1024;
const MAX_REDIRECTS: usize = 3;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);
const USER_AGENT: &str = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Page {
    pub status: u16,
    pub body: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FetchError {
    /// Адрес ведёт во внутреннюю сеть или некорректен — не загружаем никогда.
    Forbidden(String),
    /// Сеть/таймаут: попробовать позже.
    Transport(String),
}

impl std::fmt::Display for FetchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Forbidden(m) | Self::Transport(m) => f.write_str(m),
        }
    }
}

#[async_trait]
pub trait PageFetcher: Send + Sync {
    /// `cookies` — готовая строка заголовка Cookie; `proxy` — URL прокси.
    async fn fetch(&self, url: &str, cookies: Option<&str>, proxy: Option<&str>) -> Result<Page, FetchError>;
}

/// Публичный ли адрес. Всё внутреннее (loopback, частные сети, link-local, CGNAT, ULA, multicast) — нет.
pub fn is_public_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => is_public_v4(v4),
        IpAddr::V6(v6) => match v6.to_ipv4_mapped() {
            Some(v4) => is_public_v4(v4),
            None => is_public_v6(v6),
        },
    }
}

fn is_public_v4(ip: Ipv4Addr) -> bool {
    let o = ip.octets();
    !(ip.is_loopback()
        || ip.is_private()
        || ip.is_link_local()
        || ip.is_unspecified()
        || ip.is_broadcast()
        || ip.is_multicast()
        || ip.is_documentation()
        || o[0] == 0
        || (o[0] == 100 && (64..=127).contains(&o[1]))
        || (o[0] == 192 && o[1] == 0 && o[2] == 0)
        || (o[0] == 198 && (o[1] == 18 || o[1] == 19))
        || o[0] >= 240)
}

fn is_public_v6(ip: Ipv6Addr) -> bool {
    let first = ip.segments()[0];
    !(ip.is_loopback() || ip.is_unspecified() || ip.is_multicast() || (first & 0xfe00) == 0xfc00 || (first & 0xffc0) == 0xfe80)
}

/// Реальный загрузчик. `allow_private` включают только тесты (локальный фиктивный сервер).
pub struct HttpFetcher {
    pub allow_private: bool,
}

impl HttpFetcher {
    async fn resolve(&self, url: &url::Url) -> Result<(String, SocketAddr), FetchError> {
        if !matches!(url.scheme(), "http" | "https") {
            return Err(FetchError::Forbidden("разрешены только http и https".into()));
        }
        let host = url.host_str().ok_or_else(|| FetchError::Forbidden("в ссылке нет адреса".into()))?.to_string();
        let port = url.port_or_known_default().unwrap_or(443);
        let addrs: Vec<SocketAddr> = tokio::net::lookup_host((host.as_str(), port)).await.map_err(|e| FetchError::Transport(format!("не удалось найти сайт: {e}")))?.collect();
        if addrs.is_empty() {
            return Err(FetchError::Transport("сайт не найден".into()));
        }
        if !self.allow_private && addrs.iter().any(|a| !is_public_ip(a.ip())) {
            return Err(FetchError::Forbidden("адрес ведёт во внутреннюю сеть".into()));
        }
        Ok((host, addrs[0]))
    }
}

#[async_trait]
impl PageFetcher for HttpFetcher {
    async fn fetch(&self, url: &str, cookies: Option<&str>, proxy: Option<&str>) -> Result<Page, FetchError> {
        let mut current = url::Url::parse(url).map_err(|_| FetchError::Forbidden("некорректная ссылка".into()))?;
        for _ in 0..=MAX_REDIRECTS {
            let (host, addr) = self.resolve(&current).await?;
            let mut builder = reqwest::Client::builder().redirect(reqwest::redirect::Policy::none()).timeout(REQUEST_TIMEOUT).user_agent(USER_AGENT);
            builder = match proxy {
                // Через прокси DNS решает он; проверку адреса мы уже сделали выше.
                Some(p) => builder.proxy(reqwest::Proxy::all(p).map_err(|_| FetchError::Forbidden("некорректный прокси".into()))?),
                // Без прокси фиксируем проверенный адрес: защита от подмены DNS между проверкой и запросом.
                None => builder.resolve(&host, addr),
            };
            let client = builder.build().map_err(|e| FetchError::Transport(e.to_string()))?;
            let mut request = client.get(current.clone());
            if let Some(c) = cookies {
                request = request.header(reqwest::header::COOKIE, c);
            }
            let response = request.send().await.map_err(|e| FetchError::Transport(format!("сайт не отвечает: {e}")))?;
            let status = response.status();
            if status.is_redirection() {
                let next = response.headers().get(reqwest::header::LOCATION).and_then(|v| v.to_str().ok()).and_then(|l| current.join(l).ok());
                match next {
                    Some(n) => {
                        current = n;
                        continue;
                    }
                    None => return Err(FetchError::Transport("некорректное перенаправление".into())),
                }
            }
            let bytes = response.bytes().await.map_err(|e| FetchError::Transport(e.to_string()))?;
            let limited = &bytes[..bytes.len().min(MAX_BODY_BYTES)];
            return Ok(Page { status: status.as_u16(), body: String::from_utf8_lossy(limited).into_owned() });
        }
        Err(FetchError::Transport("слишком много перенаправлений".into()))
    }
}

/// Не чаще одного запроса к сайту раз в `min_gap_ms`. Чистая функция времени — тестируется без сна.
pub struct DomainThrottle {
    min_gap_ms: i64,
    next_free: DashMap<String, i64>,
}

impl DomainThrottle {
    pub fn new(min_gap_ms: i64) -> Self {
        Self { min_gap_ms, next_free: DashMap::new() }
    }

    /// Сколько миллисекунд подождать перед запросом; слот за вызывающим закрепляется сразу.
    pub fn reserve(&self, host: &str, now_ms: i64) -> i64 {
        let mut slot = self.next_free.entry(host.to_ascii_lowercase()).or_insert(0);
        let start = (*slot).max(now_ms);
        *slot = start + self.min_gap_ms;
        start - now_ms
    }
}

// ---------------------------------------------------------------------------------------------
// Пул прокси
// ---------------------------------------------------------------------------------------------

const FAILS_BEFORE_COOLDOWN: i64 = 3;
const BASE_COOLDOWN_SECS: i64 = 300;
const MAX_COOLDOWN_SECS: i64 = 6 * 3600;

pub async fn add_proxy(pool: &SqlitePool, proxy_url: &str) -> AppResult<i64> {
    let parsed = url::Url::parse(proxy_url).map_err(|_| AppError::Validation("некорректный адрес прокси".into()))?;
    if !matches!(parsed.scheme(), "http" | "https" | "socks5" | "socks5h") || parsed.host_str().is_none() {
        return Err(AppError::Validation("прокси: поддерживаются http, https и socks5".into()));
    }
    let id: i64 = sqlx::query_scalar("INSERT INTO proxies (proxy_url) VALUES (?) ON CONFLICT(proxy_url) DO UPDATE SET is_active = 1 RETURNING id")
        .bind(proxy_url)
        .fetch_one(pool)
        .await?;
    Ok(id)
}

/// Свободный прокси, которым давно не пользовались. Выбор и пометка «занят» — одним запросом.
pub async fn pick_proxy(pool: &SqlitePool, now: i64) -> AppResult<Option<(i64, String)>> {
    let row: Option<(i64, String)> = sqlx::query_as(
        "UPDATE proxies SET last_used_at = ? WHERE id = (SELECT id FROM proxies WHERE is_active = 1 AND cooldown_until <= ? \
         ORDER BY last_used_at ASC, id ASC LIMIT 1) RETURNING id, proxy_url",
    )
    .bind(now)
    .bind(now)
    .fetch_optional(pool)
    .await?;
    Ok(row)
}

pub async fn report_proxy(pool: &SqlitePool, proxy_id: i64, ok: bool, now: i64) -> AppResult<()> {
    if ok {
        sqlx::query("UPDATE proxies SET fail_count = 0, cooldown_until = 0 WHERE id = ?").bind(proxy_id).execute(pool).await?;
        return Ok(());
    }
    let fails: Option<i64> = sqlx::query_scalar("UPDATE proxies SET fail_count = fail_count + 1 WHERE id = ? RETURNING fail_count").bind(proxy_id).fetch_optional(pool).await?;
    if let Some(fails) = fails.filter(|f| *f >= FAILS_BEFORE_COOLDOWN) {
        let exponent = (fails - FAILS_BEFORE_COOLDOWN).clamp(0, 10) as u32;
        let cooldown = (BASE_COOLDOWN_SECS << exponent).min(MAX_COOLDOWN_SECS);
        sqlx::query("UPDATE proxies SET cooldown_until = ? WHERE id = ?").bind(now + cooldown).bind(proxy_id).execute(pool).await?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn internal_addresses_are_not_public() {
        for ip in ["127.0.0.1", "10.0.0.5", "172.16.3.1", "192.168.1.1", "169.254.169.254", "0.0.0.0", "100.64.0.1", "224.0.0.1", "255.255.255.255", "::1", "fe80::1", "fc00::1", "::ffff:127.0.0.1", "::ffff:10.1.1.1", "::"] {
            assert!(!is_public_ip(ip.parse().unwrap()), "{ip}");
        }
        for ip in ["8.8.8.8", "93.184.216.34", "2606:4700::1111"] {
            assert!(is_public_ip(ip.parse().unwrap()), "{ip}");
        }
    }

    #[tokio::test]
    async fn fetcher_refuses_internal_targets_and_bad_schemes() {
        let f = HttpFetcher { allow_private: false };
        for url in ["http://127.0.0.1:9/", "http://localhost/", "http://169.254.169.254/latest/meta-data", "file:///etc/passwd", "ftp://example.com/", "nonsense"] {
            assert!(matches!(f.fetch(url, None, None).await, Err(FetchError::Forbidden(_))), "{url}");
        }
    }

    #[tokio::test]
    async fn fetcher_reads_page_sends_cookies_and_follows_redirects_with_recheck() {
        use crate::testutil::MockServer;
        let server = MockServer::start().await;
        server.redirect("/old", "/new");
        server.reply("/new", 200, "hello");
        let f = HttpFetcher { allow_private: true };
        let page = f.fetch(&format!("{}/old", server.base_url), Some("sid=1"), None).await.unwrap();
        assert_eq!((page.status, page.body.as_str()), (200, "hello"));
        assert_eq!(server.requests_to("/new")[0].headers.get("cookie").map(String::as_str), Some("sid=1"));
        let strict = HttpFetcher { allow_private: false };
        assert!(strict.fetch(&format!("{}/old", server.base_url), None, None).await.is_err());
    }

    #[test]
    fn throttle_spaces_requests_per_domain() {
        let t = DomainThrottle::new(3_000);
        assert_eq!(t.reserve("A.example", 1_000), 0);
        assert_eq!(t.reserve("a.example", 1_000), 3_000);
        assert_eq!(t.reserve("a.example", 1_000), 6_000);
        assert_eq!(t.reserve("b.example", 1_000), 0);
        assert_eq!(t.reserve("a.example", 20_000), 0);
    }

    async fn pool() -> (tempfile::TempDir, SqlitePool) {
        let dir = tempfile::tempdir().unwrap();
        let pool = crate::db::init_pool(&dir.path().join("p.db")).await.unwrap();
        (dir, pool)
    }

    #[tokio::test]
    async fn proxies_rotate_cool_down_and_recover() {
        let (_d, pool) = pool().await;
        assert!(add_proxy(&pool, "ftp://x").await.is_err());
        let a = add_proxy(&pool, "http://p1.example:8080").await.unwrap();
        let b = add_proxy(&pool, "socks5://p2.example:1080").await.unwrap();
        assert_eq!(add_proxy(&pool, "http://p1.example:8080").await.unwrap(), a, "повторное добавление не плодит строки");
        assert_eq!(pick_proxy(&pool, 100).await.unwrap().unwrap().0, a);
        assert_eq!(pick_proxy(&pool, 101).await.unwrap().unwrap().0, b, "ротация: давно не использованный");
        for _ in 0..3 {
            report_proxy(&pool, a, false, 200).await.unwrap();
        }
        assert_eq!(pick_proxy(&pool, 300).await.unwrap().unwrap().0, b, "a на паузе");
        assert_eq!(pick_proxy(&pool, 301).await.unwrap().unwrap().0, b);
        assert_eq!(pick_proxy(&pool, 200 + 300 + 1).await.unwrap().unwrap().0, a, "пауза закончилась");
        report_proxy(&pool, a, true, 600).await.unwrap();
        let fails: i64 = sqlx::query_scalar("SELECT fail_count FROM proxies WHERE id = ?").bind(a).fetch_one(&pool).await.unwrap();
        assert_eq!(fails, 0);
    }

    #[tokio::test]
    async fn no_proxies_means_direct_connection() {
        let (_d, pool) = pool().await;
        assert!(pick_proxy(&pool, 1).await.unwrap().is_none());
    }
}
