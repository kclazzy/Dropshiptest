fn main() {
    // Миграции встраиваются в бинарник макросом sqlx::migrate!: пересобираем при их изменении.
    println!("cargo:rerun-if-changed=migrations");
    tauri_build::build();
}
