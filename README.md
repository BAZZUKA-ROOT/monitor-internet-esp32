# 📡 Monitor de Internet ESP32

Monitor de conectividade WiFi/Internet com display de alertas via Telegram — projeto Arduino/ESP32.

![Language](https://img.shields.io/badge/Stack-C%2B%2B%20%7C%20Arduino-blue) ![Platform](https://img.shields.io/badge/Platform-ESP32-red) ![License](https://img.shields.io/badge/Licen%C3%A7a-MIT-blue)

---

## 🎯 Recursos

- Teste de internet em Google, Microsoft e Cloudflare
- Medição de latência, jitter e taxa de falhas
- Classificação automática: Excelente / Boa / Regular / Ruim / Crítica
- Histórico persistente dos últimos 80 eventos (salvo em NVS/flash)
- Portal web responsivo para configuração via WiFi
- Notificações e comandos via Telegram
- OTA (atualização over-the-air)
- Relatório diário configurável
- Alerta de temperatura interna do ESP32

## 📟 Comandos Telegram

| Comando | Ação |
|---|---|
| `/status` | Estado atual da internet |
| `/relatorio` | Relatório completo |
| `/ping` | Teste de ping |
| `/historico` | Últimos 80 eventos |
| `/painel` | Link para painel web |
| `/ajuda` | Lista de comandos |

## 🔧 Como configurar

1. Abra no Arduino IDE (ou PlatformIO)
2. Instale as bibliotecas: `WiFi`, `WebServer`, `Preferences`, `HTTPClient`
3. Grave no ESP32
4. Conecte-se à rede `ESP32-Monitor` (senha: `12345678`)
5. Configure WiFi e Token do Telegram pelo portal web

## 📦 Dependências Arduino

- ESP32 Board Package
- WiFi, WebServer, DNSServer (incluídas no ESP32 package)
- HTTPClient, WiFiClientSecure
- Preferences (NVS)

## 📄 Licença

MIT

---
Feito com ☕ no Brasil 🇧🇷
