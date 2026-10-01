<p align="center">
  <img src="docs/readme_logo.svg" />
</p>

# VKPCNX

[![Claude Code](https://img.shields.io/badge/Claude%20Code-D97757?logo=claude&logoColor=white)](https://claude.com/claude-code) [![Borealis UI](https://img.shields.io/badge/Borealis%20UI-1E88E5?logo=github&logoColor=white)](https://github.com/xfangfang/borealis) [![Switch Homebrew](https://img.shields.io/badge/Switch%20Homebrew-E60012?logo=retroarch&logoColor=white)](https://switchbrew.org/) [![Build](https://img.shields.io/github/actions/workflow/status/m4l3vich/vkpcnx/ci.yaml?branch=main&logo=githubactions&logoColor=white&label=build)](https://github.com/m4l3vich/vkpcnx/actions/workflows/ci.yaml)

[English](./README_EN.md) | [Telegram-канал](https://t.me/vkpcnx)

VK Play Cloud NX — homebrew-клиент сервиса облачного гейминга VK Play Cloud для прошитых Nintendo Switch, разработанный с помощью реверс-инжиниринга официального веб-клиента. Поддерживает всё необходимое для игры на ПК в облаке через Switch. Выполнен в нативном стиле интерфейса Nintendo Switch благодаря библиотеке [Borealis (форк от xfangfang)](https://github.com/xfangfang/borealis). Содержит баги и вайбкод.

## Установка

Скачайте `vkpcnx.nro` на странице [Releases](releases/) и поместите его в папку `switch` на SD-карте. Откройте приложение  "VK Play Cloud NX" в hbmenu и следуйте инструкциям для входа в аккаунт и выполнения первоначальной настройки.

> [!IMPORTANT]
> В приложении нельзя осуществлять платежи и изменять настройки аккаунта VK Play — это намеренное решение. Используйте веб-браузер на другом устройстве.

## Обратная связь

Если у вас возникнут проблемы при использовании клиента, или если вы хотите предложить что-то, что можно добавить в приложение, напишите об этом либо в [issues здесь, на GitHub](issues/), либо в [личные сообщения Telegram-канала VKPCNX](https://t.me/vkpcnx?direct).

При возникновении ошибок в работе приложения, пожалуйста, соберите логи и отчёты об ошибках приложения:

- **Клиент крашится при запуске** — приложите к вашему сообщению последние (самые новые) файлы из папок `config/vkpcnx/logs/` и `atmosphere/crash-reports/` на SD-карте.
- **Клиент вылетел или завис во время работы** — при следующем запуске оно само предложит создать отчёт об ошибке, нажмите «Создать отчёт».
- **Возникла ошибка при использовании клиента** — подробно опишите ошибку и условия её возникновения (желательно прикрепить скриншоты или видеозапись экрана), а также сгенерируйте отчёт в приложении (Главный экран → Настройки аккаунта → Обратная связь → Создать отчёт об ошибке), он будет сохранён в папку `config/vkpcnx/reports/` на SD-карте.

> [!IMPORTANT]
> При записи логов приложение автоматически скрывает все личные данные (пароли, адреса электронной почти, ключи доступа, IP-адреса).

## Разработка и сборка приложения (Linux/macOS)

Подробное описание протокола VK Play Cloud / результаты ИИ-реверс-инжиниринга официального веб-клиента можно найти в файле [docs/VKPC_PROTO.md](docs/VKPC_PROTO.md).

Для сборки приложения вам потребуются зависимости: `ffmpeg opus openssl@3 zlib` и `cmake` как минимум версии 3.21.

Чтобы собрать приложение на десктоп, запустите скрипт `./scripts/build-desktop.sh`. Готовый исполняемый файл будет  доступен в `./build/vkpcnx`

Для запуска юнит-тестов:

```bash
cmake -B build -DPLATFORM_DESKTOP=ON -DVKPCNX_BUILD_TESTS=ON && cmake --build build --target stream_protocol_test && ./build/stream_protocol_test
```

Для сборки под Switch есть два варианта:

- **Нативный devkitPro**: понадобятся установленный devkitPro и `switch-dev switch-curl switch-zlib switch-libopus
  switch-ffmpeg switch-libwebp switch-glfw switch-mesa switch-libdrm_nouveau`
  Запустить сборку: `./scripts/build-switch.sh`
- **Через Docker**: понадобится установленный Docker, для запуска сборки используйте:
  `./scripts/build-switch.sh --docker`

Для сборки дебаг-варианта добавьте аргумент `--debug` при запуске скрипта `build-switch.sh`.

Для тестирования клиента на mock-сервере:

```bash
python3 -m venv venv && venv/bin/pip install aiortc websockets protobuf av numpy
mkdir -p pb && protoc -I vkpcnx/proto --python_out=pb vkpcnx/proto/*.proto
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 30 -subj /CN=localhost
venv/bin/python tests/mock_server.py --pb pb --cert cert.pem --key key.pem --end-after 30
VKPCNX_INSECURE_TLS=1 VKPCNX_PLAY_URL='playkey:///?host=localhost&port=19000&token=test' ./build/vkpcnx
```
