// Не открывать консольное окно в релизной сборке Windows.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    if let Err(error) = dropship_lib::run() {
        eprintln!("Не удалось запустить Dropship: {error}");
        std::process::exit(1);
    }
}
