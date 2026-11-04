# Облако: ИИ-провайдеры и запуск ядра на сервере

Есть две независимые «облачные» возможности:

- **А. Облачный ИИ** — тексты карточек пишет OpenAI, Claude, OpenRouter и т. д. вместо локальной Ollama.
- **Б. Ядро на сервере (VPS)** — программа работает круглосуточно в облаке, расширение в вашем браузере подключается к ней через защищённый туннель.

Можно использовать одно, другое или оба.

## А. Облачный ИИ

Выбор провайдера — в `~/.dropship/settings.json` (перезапустите ядро после правки). **Ключ API в этот файл не пишется.** Его задают переменной окружения (или файлом `~/.dropship/secrets/ИМЯ_ПЕРЕМЕННОЙ` — одна строка с ключом; удобно для Docker secrets).

Пример: Claude.

```json
{ "ai": { "provider": "anthropic", "model": "claude-haiku-5-5", "language": "ru" } }
```

```bash
export ANTHROPIC_API_KEY="ваш-ключ"
scripts/run.sh
```

| `provider` | Переменная ключа | Модель (`model`) |
|---|---|---|
| `ollama` | не нужна | можно пусто — берётся модель по умолчанию |
| `anthropic` | `ANTHROPIC_API_KEY` | например `claude-haiku-5-5` (дёшево) или `claude-sonnet-5-5` (качественнее) |
| `openai` | `OPENAI_API_KEY` | любая модель вашего аккаунта с поддержкой структурного вывода |
| `openrouter` | `OPENROUTER_API_KEY` | в формате `производитель/модель` из каталога OpenRouter |
| `groq` | `GROQ_API_KEY` | из каталога Groq |
| `together` | `TOGETHER_API_KEY` | из каталога Together |
| `mistral` | `MISTRAL_API_KEY` | из каталога Mistral |
| `deepseek` | `DEEPSEEK_API_KEY` | из документации DeepSeek (адрес и режим JSON не сверены — проверьте) |
| `gemini` | `GEMINI_API_KEY` | из Google AI Studio; если модель отклоняет схему — `"json_mode": "json_object"` |
| `yandex` | `YANDEX_API_KEY` | `gpt://<ID каталога>/yandexgpt/latest` + `"extra_headers": {"OpenAI-Project": "<ID каталога>"}` |
| `custom` | `AI_API_KEY` | любой OpenAI-совместимый сервер: обязательно `"base_url": "https://…/v1"` |

Дополнительные поля: `base_url` (свой адрес), `api_key_env` (другое имя переменной), `json_mode` (`json_schema` или `json_object`), `extra_headers`.

Правила безопасности: обычный `http://` разрешён только для адресов на вашем компьютере; ключ уходит только на адрес провайдера; ошибка настроек не останавливает ядро — в логе будет «AI disabled» с причиной, а импорт товаров продолжит работать (задачи ИИ подождут в очереди).

**Что уходит в облако:** название, описание и характеристики товара поставщика. Цены, ваши заметки и куки — нет.

## Б. Ядро на сервере (VPS)

Схема: ядро на сервере слушает только `127.0.0.1:8765`; на вашем компьютере вы пробрасываете этот порт SSH-туннелем, поэтому расширение подключается к «локальному» адресу, а в интернет порт не открыт. Токен `bridge.token` по-прежнему обязателен.

### 1. Запуск в Docker (рекомендуется)

На сервере (нужны `git`, Docker):

```bash
git clone https://github.com/kclazzy/Dropshiptest.git && cd Dropshiptest
cat > .env <<'ENV'
ANTHROPIC_API_KEY=ваш-ключ
ENV
docker compose up -d --build
docker compose logs core | head      # ядро должно написать "is running"
```

Данные лежат в Docker-томе `dropship-data`. Настройки ИИ: `docker compose exec core sh -c 'cat /data/settings.json'`; для правки скопируйте файл (`docker compose cp`), измените и перезапустите (`docker compose restart`). Токен: `docker compose exec core cat /data/bridge.token`.

> Dockerfile и compose-файл написаны по документации, но **в среде разработки не собирались** (там нет работающего Docker-демона). Если сборка упадёт — пришлите вывод.

### 2. Запуск без Docker

Соберите так же, как локально (`scripts/build.sh`) и запустите под systemd:

```ini
# /etc/systemd/system/dropship.service
[Service]
User=dropship
Environment=ANTHROPIC_API_KEY=ваш-ключ
Environment=DROPSHIP_LAUNCHER=1
ExecStart=/home/dropship/Dropshiptest/core-cpp/build/dropship_core_app --data-dir /var/lib/dropship
Restart=always
```

(Кнопка обновления рассчитана на запуск через `scripts/run.sh`; на сервере обновляйтесь `git pull && scripts/build.sh && systemctl restart dropship`, а в unit-файле `DROPSHIP_LAUNCHER` не задавайте.)

### 3. Подключение расширения

На вашем компьютере держите туннель открытым:

```bash
ssh -N -L 8765:127.0.0.1:8765 user@адрес-сервера
```

Вставьте в расширение токен **с сервера**. Дальше всё работает как локально. Занят порт 8765 — выберите другой слева (`-L 9000:…`) — но тогда расширению нужен тот же порт; сейчас он зашит как 8765, поэтому проще освободить 8765.

### Чего делать не нужно

- Не открывайте порт 8765 наружу (`0.0.0.0`) и не ставьте его за обычный прокси: ядро намеренно отказывается слушать не-loopback адреса.
- Не храните ключи API в `settings.json` и в репозитории (`.env` уже в `.gitignore`).
