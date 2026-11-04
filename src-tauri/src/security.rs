//! Шифрование секретов (ключи API магазинов и ИИ-провайдеров): AES-256-GCM.
//! Ключ шифрования — файл `secret.key` в папке данных (32 случайных байта, права 0600 на Unix).
//! Повреждённый файл ключа **не заменяется молча**: иначе все сохранённые секреты стали бы нечитаемыми без предупреждения.

use std::path::Path;

use aes_gcm::aead::{Aead, AeadCore, KeyInit, OsRng, Payload};
use aes_gcm::{Aes256Gcm, Key, Nonce};

use crate::error::{AppError, AppResult};

const KEY_LEN: usize = 32;
const NONCE_LEN: usize = 12;
/// Привязка шифротекста к назначению: блок из другой подсистемы не расшифруется.
const AAD: &[u8] = b"dropship-secrets-v1";

pub struct CredentialVault {
    cipher: Aes256Gcm,
}

impl CredentialVault {
    pub fn from_key(key: [u8; KEY_LEN]) -> Self {
        Self { cipher: Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(&key)) }
    }

    /// Читает ключ из файла или создаёт новый, если файла нет.
    pub fn open_or_create(path: &Path) -> AppResult<Self> {
        match std::fs::read(path) {
            Ok(bytes) => {
                let key: [u8; KEY_LEN] = bytes.try_into().map_err(|_| {
                    AppError::Validation("файл secret.key повреждён: восстановите его из резервной копии".into())
                })?;
                Ok(Self::from_key(key))
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
                    std::fs::create_dir_all(parent)?;
                }
                let key = Aes256Gcm::generate_key(OsRng);
                write_private(path, key.as_slice())?;
                let mut bytes = [0u8; KEY_LEN];
                bytes.copy_from_slice(key.as_slice());
                Ok(Self::from_key(bytes))
            }
            Err(e) => Err(e.into()),
        }
    }

    /// `nonce(12) | шифротекст | тег(16)`; nonce каждый раз новый.
    pub fn encrypt(&self, plaintext: &[u8]) -> AppResult<Vec<u8>> {
        let nonce = Aes256Gcm::generate_nonce(&mut OsRng);
        let ciphertext = self
            .cipher
            .encrypt(&nonce, Payload { msg: plaintext, aad: AAD })
            .map_err(|_| AppError::Validation("не удалось зашифровать данные".into()))?;
        let mut out = nonce.to_vec();
        out.extend_from_slice(&ciphertext);
        Ok(out)
    }

    pub fn decrypt(&self, blob: &[u8]) -> AppResult<Vec<u8>> {
        if blob.len() < NONCE_LEN + 16 {
            return Err(AppError::Validation("сохранённые ключи повреждены".into()));
        }
        let (nonce, ciphertext) = blob.split_at(NONCE_LEN);
        self.cipher
            .decrypt(Nonce::from_slice(nonce), Payload { msg: ciphertext, aad: AAD })
            .map_err(|_| AppError::Validation("не удалось расшифровать ключи: возможно, заменён файл secret.key".into()))
    }
}

#[cfg(unix)]
fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let mut file = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(path)?;
    file.write_all(bytes)
}

#[cfg(not(unix))]
fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    let mut file = std::fs::OpenOptions::new().write(true).create_new(true).open(path)?;
    file.write_all(bytes)
}

pub fn hex_encode(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn hex_decode(text: &str) -> Option<Vec<u8>> {
    if text.len() % 2 != 0 || !text.is_ascii() {
        return None;
    }
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).ok()).collect()
}

/// 32 случайных байта в hex (64 символа): токены моста и одноразовые коды.
pub fn random_token() -> String {
    hex_encode(Aes256Gcm::generate_key(OsRng).as_slice())
}

/// SHA-256 в hex: в базе хранится только отпечаток токена, не сам токен.
pub fn sha256_hex(text: &str) -> String {
    use sha2::{Digest, Sha256};
    hex_encode(&Sha256::digest(text.as_bytes()))
}

/// Сравнение без раннего выхода по первому несовпавшему байту.
pub fn constant_time_eq(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    let mut diff = (a.len() ^ b.len()) as u8;
    for i in 0..a.len().max(b.len()) {
        diff |= a.get(i).copied().unwrap_or(0) ^ b.get(i).copied().unwrap_or(0);
    }
    diff == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_and_fresh_nonce() {
        let vault = CredentialVault::from_key([7; 32]);
        let a = vault.encrypt(b"secret").unwrap();
        let b = vault.encrypt(b"secret").unwrap();
        assert_ne!(a, b);
        assert!(!a.windows(6).any(|w| w == b"secret"));
        assert_eq!(vault.decrypt(&a).unwrap(), b"secret");
    }

    #[test]
    fn tampering_wrong_key_and_short_blobs_fail() {
        let vault = CredentialVault::from_key([7; 32]);
        let mut blob = vault.encrypt(b"secret").unwrap();
        assert!(CredentialVault::from_key([8; 32]).decrypt(&blob).is_err());
        assert!(vault.decrypt(&blob[..10]).is_err());
        let last = blob.len() - 1;
        blob[last] ^= 1;
        assert!(vault.decrypt(&blob).is_err());
    }

    #[test]
    fn key_file_is_created_reused_and_never_silently_replaced() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("nested").join("secret.key");
        let first = CredentialVault::open_or_create(&path).unwrap();
        let blob = first.encrypt(b"x").unwrap();
        assert_eq!(CredentialVault::open_or_create(&path).unwrap().decrypt(&blob).unwrap(), b"x");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(std::fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        }
        std::fs::write(&path, b"short").unwrap();
        assert!(CredentialVault::open_or_create(&path).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), b"short");
    }

    #[test]
    fn hex_helpers() {
        assert_eq!(hex_encode(&[0, 15, 255]), "000fff");
        assert_eq!(hex_decode("000fff"), Some(vec![0, 15, 255]));
        assert_eq!(hex_decode("0"), None);
        assert_eq!(hex_decode("zz"), None);
        assert_eq!(hex_decode("é1"), None);
    }
}

