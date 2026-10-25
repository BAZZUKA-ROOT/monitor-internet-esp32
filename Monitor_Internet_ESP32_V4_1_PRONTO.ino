/*
  ================================================================
  MONITOR DE INTERNET ESP32 - VERSÃO 4.0
  ================================================================

  RECURSOS
  - Mensagem diferenciada sempre que o ESP32 ligar
  - Identificação do motivo do reset
  - Teste da internet em Google, Microsoft e Cloudflare
  - Medição de latência, jitter e taxa de falhas
  - Classificação automática: Excelente, Boa, Regular, Ruim, Crítica
  - Estatísticas da sessão e acumuladas
  - Histórico persistente dos últimos 80 eventos
  - Relatório diário configurável
  - Temperatura interna aproximada do ESP32
  - Alertas de internet lenta, sinal Wi-Fi fraco e temperatura alta
  - Portal web responsivo
  - Comandos Telegram sem repetir mensagens antigas
  - OTA
  - Sem reinício semanal e sem reinício apenas por ficar offline

  COMANDOS DO TELEGRAM
  /status
  /relatorio
  /ping
  /historico
  /painel
  /ajuda

  OBSERVAÇÃO
  A temperatura retornada por temperatureRead() é a temperatura interna
  aproximada do chip, não a temperatura exata do ambiente.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <esp_system.h>

// ================================================================
// CONFIGURAÇÕES GERAIS
// ================================================================

const char* VERSAO_FIRMWARE = "4.0";
const char* AP_NOME = "MonitorESP32";
const char* AP_SENHA = "12345678";

const char* SSID_PADRAO = "Quarto👍🏻";
const char* CHAT_ID_PADRAO = "1662803142";

const int LED_PIN = 2;

const unsigned long INTERVALO_TESTE = 30000UL;
const unsigned long INTERVALO_BOT = 7000UL;
const unsigned long INTERVALO_RECONEXAO_WIFI = 20000UL;

const int FALHAS_PARA_QUEDA = 3;
const int MAX_HISTORICO = 80;

// Alertas
const unsigned long LATENCIA_ALERTA_MS = 250;
const int RSSI_ALERTA_DBM = -78;
const float TEMPERATURA_ALERTA_C = 75.0;
const unsigned long INTERVALO_REPETIR_ALERTA = 60UL * 60UL * 1000UL;

// ================================================================
// OBJETOS
// ================================================================

WebServer servidor(80);
DNSServer dns;
Preferences memoria;

// ================================================================
// CONFIGURAÇÕES SALVAS
// ================================================================

String wifiNome;
String wifiSenha;
String botToken;
String chatId;
String nomeEquipamento = "Monitor ESP32";

bool relatorioDiarioAtivo = true;
int relatorioHora = 20;
int relatorioMinuto = 0;

// ================================================================
// ESTADO
// ================================================================

bool modoConfiguracao = false;
bool internetOffline = false;
bool primeiraVerificacao = true;

int falhasConsecutivas = 0;
int quantidadeQuedasSessao = 0;
uint32_t quantidadeQuedasTotal = 0;

unsigned long ultimoTeste = 0;
unsigned long ultimaConsultaBot = 0;
unsigned long ultimaTentativaWiFi = 0;
unsigned long inicioQueda = 0;
unsigned long ultimaPiscada = 0;

unsigned long ultimaLatencia = 0;
unsigned long menorLatencia = 0;
unsigned long maiorLatencia = 0;
uint64_t somaLatencias = 0;
uint32_t quantidadeLatencias = 0;

unsigned long latenciaAnterior = 0;
uint64_t somaJitter = 0;
uint32_t quantidadeJitter = 0;

uint32_t testesTotal = 0;
uint32_t testesFalhos = 0;
uint64_t totalOfflineSegundos = 0;

String ultimaQueda = "Nenhuma";
String ultimoRetorno = "Nenhum";
String ultimaDuracao = "Nenhuma";
String ultimoMotivoQueda = "Nenhum";
String ultimoServidorOK = "Nenhum";
String ultimoRelatorioData = "";

long ultimoUpdateTelegram = 0;

unsigned long ultimoAlertaLatencia = 0;
unsigned long ultimoAlertaRSSI = 0;
unsigned long ultimoAlertaTemperatura = 0;

String historico[MAX_HISTORICO];
int historicoInicio = 0;
int historicoQuantidade = 0;

// ================================================================
// ESTRUTURAS
// ================================================================

struct ResultadoInternet {
  bool online;
  String servidor;
  unsigned long latencia;
  int sucessos;
  int falhas;
};

// ================================================================
// FUNÇÕES AUXILIARES
// ================================================================

String codificarURL(const String& texto) {
  String resultado;
  char hexadecimal[4];

  for (unsigned int i = 0; i < texto.length(); i++) {
    unsigned char c = texto.charAt(i);

    if (
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~'
    ) {
      resultado += (char)c;
    } else {
      snprintf(hexadecimal, sizeof(hexadecimal), "%%%02X", c);
      resultado += hexadecimal;
    }
  }

  return resultado;
}

String escaparHTML(String texto) {
  texto.replace("&", "&amp;");
  texto.replace("<", "&lt;");
  texto.replace(">", "&gt;");
  texto.replace("\"", "&quot;");
  texto.replace("'", "&#39;");
  return texto;
}

String decodificarJSONSimples(String texto) {
  texto.replace("\\n", "\n");
  texto.replace("\\\"", "\"");
  texto.replace("\\/", "/");
  texto.replace("\\\\", "\\");
  return texto;
}

String obterDataHora() {
  struct tm horario;

  if (!getLocalTime(&horario, 1200)) {
    return "Horário indisponível";
  }

  char texto[32];
  strftime(texto, sizeof(texto), "%d/%m/%Y às %H:%M:%S", &horario);
  return String(texto);
}

String obterDataISO() {
  struct tm horario;

  if (!getLocalTime(&horario, 800)) {
    return "";
  }

  char texto[12];
  strftime(texto, sizeof(texto), "%Y-%m-%d", &horario);
  return String(texto);
}

String obterDataHoraCurta() {
  struct tm horario;

  if (!getLocalTime(&horario, 500)) {
    return String(millis() / 1000UL) + "s";
  }

  char texto[22];
  strftime(texto, sizeof(texto), "%d/%m %H:%M:%S", &horario);
  return String(texto);
}

String formatarDuracaoSegundos(uint64_t segundos) {
  uint64_t dias = segundos / 86400ULL;
  uint64_t horas = (segundos % 86400ULL) / 3600ULL;
  uint64_t minutos = (segundos % 3600ULL) / 60ULL;
  uint64_t resto = segundos % 60ULL;

  String resultado;

  if (dias > 0) resultado += String((unsigned long)dias) + "d ";
  if (horas > 0 || dias > 0) resultado += String((unsigned long)horas) + "h ";
  if (minutos > 0 || horas > 0 || dias > 0) resultado += String((unsigned long)minutos) + "min ";

  resultado += String((unsigned long)resto) + "s";
  return resultado;
}

String formatarUptime() {
  return formatarDuracaoSegundos(millis() / 1000UL);
}

String nomeMotivoReset() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "PowerOn / energia ligada";
    case ESP_RST_EXT:       return "Reset externo";
    case ESP_RST_SW:        return "Software / reinicialização manual";
    case ESP_RST_PANIC:     return "Falha crítica / Panic";
    case ESP_RST_INT_WDT:   return "Watchdog interno";
    case ESP_RST_TASK_WDT:  return "Watchdog de tarefa";
    case ESP_RST_WDT:       return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Saída do Deep Sleep";
    case ESP_RST_BROWNOUT:  return "Brownout / queda de tensão";
    default:                return "Motivo desconhecido";
  }
}

float lerTemperaturaESP32() {
  return temperatureRead();
}

unsigned long latenciaMedia() {
  if (quantidadeLatencias == 0) return 0;
  return (unsigned long)(somaLatencias / quantidadeLatencias);
}

unsigned long jitterMedio() {
  if (quantidadeJitter == 0) return 0;
  return (unsigned long)(somaJitter / quantidadeJitter);
}

float taxaFalhas() {
  if (testesTotal == 0) return 0.0;
  return ((float)testesFalhos * 100.0) / (float)testesTotal;
}

float disponibilidadeSessao() {
  if (testesTotal == 0) return 100.0;
  return 100.0 - taxaFalhas();
}

String classificarQualidade(unsigned long latencia, unsigned long jitter, float falhas) {
  if (internetOffline) return "Sem internet";
  if (falhas >= 30.0 || latencia > 500 || jitter > 180) return "Crítica";
  if (falhas >= 15.0 || latencia > 250 || jitter > 100) return "Ruim";
  if (falhas >= 5.0 || latencia > 120 || jitter > 50) return "Regular";
  if (latencia > 50 || jitter > 25) return "Boa";
  return "Excelente";
}

String estrelasQualidade(const String& qualidade) {
  if (qualidade == "Excelente") return "★★★★★";
  if (qualidade == "Boa") return "★★★★☆";
  if (qualidade == "Regular") return "★★★☆☆";
  if (qualidade == "Ruim") return "★★☆☆☆";
  if (qualidade == "Crítica") return "★☆☆☆☆";
  return "☆☆☆☆☆";
}

// ================================================================
// MEMÓRIA
// ================================================================

void salvarEstatisticasPersistentes() {
  memoria.begin("monitor4", false);
  memoria.putUInt("quedasTotal", quantidadeQuedasTotal);
  memoria.putULong64("offlineTotal", totalOfflineSegundos);
  memoria.putString("ultimaQueda", ultimaQueda);
  memoria.putString("ultimoRet", ultimoRetorno);
  memoria.putString("ultimaDur", ultimaDuracao);
  memoria.putString("motivoQueda", ultimoMotivoQueda);
  memoria.end();
}

void salvarUltimoUpdateTelegram() {
  memoria.begin("monitor4", false);
  memoria.putLong("telegramUpd", ultimoUpdateTelegram);
  memoria.end();
}

void salvarHistoricoCompleto() {
  memoria.begin("historico4", false);
  memoria.clear();
  memoria.putInt("inicio", historicoInicio);
  memoria.putInt("quantidade", historicoQuantidade);

  for (int i = 0; i < MAX_HISTORICO; i++) {
    String chave = "h" + String(i);
    memoria.putString(chave.c_str(), historico[i]);
  }

  memoria.end();
}

void adicionarHistorico(const String& tipo, const String& descricao) {
  String linha = obterDataHoraCurta() + " | " + tipo + " | " + descricao;

  int indice;

  if (historicoQuantidade < MAX_HISTORICO) {
    indice = (historicoInicio + historicoQuantidade) % MAX_HISTORICO;
    historicoQuantidade++;
  } else {
    indice = historicoInicio;
    historicoInicio = (historicoInicio + 1) % MAX_HISTORICO;
  }

  historico[indice] = linha;
  Serial.println(linha);
  salvarHistoricoCompleto();
}

void carregarConfiguracoes() {
  // Primeiro tenta carregar a configuração da versão 4.
  memoria.begin("monitor4", true);

  wifiNome = memoria.getString("ssid", "");
  wifiSenha = memoria.getString("senha", "");
  botToken = memoria.getString("token", "");
  chatId = memoria.getString("chatid", CHAT_ID_PADRAO);
  nomeEquipamento = memoria.getString("nome", "Monitor de Internet ESP32");

  relatorioDiarioAtivo = memoria.getBool("relAtivo", true);
  relatorioHora = memoria.getInt("relHora", 20);
  relatorioMinuto = memoria.getInt("relMin", 0);

  quantidadeQuedasTotal = memoria.getUInt("quedasTotal", 0);
  totalOfflineSegundos = memoria.getULong64("offlineTotal", 0);
  ultimaQueda = memoria.getString("ultimaQueda", "Nenhuma");
  ultimoRetorno = memoria.getString("ultimoRet", "Nenhum");
  ultimaDuracao = memoria.getString("ultimaDur", "Nenhuma");
  ultimoMotivoQueda = memoria.getString("motivoQueda", "Nenhum");
  ultimoRelatorioData = memoria.getString("relData", "");
  ultimoUpdateTelegram = memoria.getLong("telegramUpd", 0);

  memoria.end();

  // Migração automática da versão anterior.
  // Assim o Wi-Fi, senha, token e Chat ID que já estão gravados
  // no ESP32 são reaproveitados sem precisar preencher novamente.
  bool precisaMigrar =
    wifiNome.isEmpty() ||
    wifiSenha.isEmpty() ||
    botToken.isEmpty() ||
    chatId.isEmpty();

  if (precisaMigrar) {
    memoria.begin("monitor", true);

    String wifiAntigo = memoria.getString("ssid", SSID_PADRAO);
    String senhaAntiga = memoria.getString("senha", "");
    String tokenAntigo = memoria.getString("token", "");
    String chatAntigo = memoria.getString("chatid", CHAT_ID_PADRAO);

    bool relatorioAntigoAtivo = memoria.getBool("relativo", true);
    int horaAntiga = memoria.getInt("relhora", 20);
    int minutoAntigo = memoria.getInt("relmin", 0);

    memoria.end();

    if (!wifiAntigo.isEmpty()) wifiNome = wifiAntigo;
    if (!senhaAntiga.isEmpty()) wifiSenha = senhaAntiga;
    if (!tokenAntigo.isEmpty()) botToken = tokenAntigo;
    if (!chatAntigo.isEmpty()) chatId = chatAntigo;

    relatorioDiarioAtivo = relatorioAntigoAtivo;
    relatorioHora = horaAntiga;
    relatorioMinuto = minutoAntigo;
    nomeEquipamento = "Monitor de Internet ESP32";

    // Grava a configuração migrada no formato novo.
    salvarConfiguracoes();
  }

  memoria.begin("historico4", true);
  historicoInicio = memoria.getInt("inicio", 0);
  historicoQuantidade = memoria.getInt("quantidade", 0);

  if (historicoInicio < 0 || historicoInicio >= MAX_HISTORICO) historicoInicio = 0;
  if (historicoQuantidade < 0 || historicoQuantidade > MAX_HISTORICO) historicoQuantidade = 0;

  for (int i = 0; i < MAX_HISTORICO; i++) {
    String chave = "h" + String(i);
    historico[i] = memoria.getString(chave.c_str(), "");
  }

  memoria.end();

  if (relatorioHora < 0 || relatorioHora > 23) relatorioHora = 20;
  if (relatorioMinuto < 0 || relatorioMinuto > 59) relatorioMinuto = 0;
}

void salvarConfiguracoes() {
  memoria.begin("monitor4", false);

  memoria.putString("ssid", wifiNome);
  memoria.putString("senha", wifiSenha);
  memoria.putString("token", botToken);
  memoria.putString("chatid", chatId);
  memoria.putString("nome", nomeEquipamento);

  memoria.putBool("relAtivo", relatorioDiarioAtivo);
  memoria.putInt("relHora", relatorioHora);
  memoria.putInt("relMin", relatorioMinuto);

  memoria.end();
}

void salvarDataRelatorio(const String& data) {
  ultimoRelatorioData = data;

  memoria.begin("monitor4", false);
  memoria.putString("relData", data);
  memoria.end();
}

// ================================================================
// TELEGRAM
// ================================================================

bool enviarTelegram(const String& mensagem) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (botToken.length() < 20 || chatId.length() < 3) return false;

  WiFiClientSecure cliente;
  cliente.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(10000);

  String url = "https://api.telegram.org/bot" + botToken + "/sendMessage";

  if (!http.begin(cliente, url)) return false;

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String dados =
    "chat_id=" + chatId +
    "&text=" + codificarURL(mensagem);

  int codigo = http.POST(dados);
  http.end();

  return codigo >= 200 && codigo < 300;
}

String extrairValorJSON(const String& json, const String& chave, int inicioBusca = 0) {
  int pos = json.indexOf(chave, inicioBusca);
  if (pos < 0) return "";

  pos += chave.length();

  while (pos < (int)json.length() && (json[pos] == ' ' || json[pos] == ':')) pos++;
  if (pos >= (int)json.length()) return "";

  if (json[pos] == '"') {
    pos++;
    int fim = pos;

    while (fim < (int)json.length()) {
      if (json[fim] == '"' && json[fim - 1] != '\\') break;
      fim++;
    }

    return decodificarJSONSimples(json.substring(pos, fim));
  }

  int fim = pos;

  while (
    fim < (int)json.length() &&
    json[fim] != ',' &&
    json[fim] != '}' &&
    json[fim] != ']'
  ) {
    fim++;
  }

  String valor = json.substring(pos, fim);
  valor.trim();
  return valor;
}

String montarMensagemInicial() {
  return
    "🟢 MONITOR LIGADO\n\n"
    "O equipamento foi iniciado.\n\n"
    "Possíveis motivos:\n"
    "• Energia restabelecida\n"
    "• Reinicialização manual\n"
    "• Atualização OTA\n"
    "• Reinício automático\n\n"
    "Equipamento: " + nomeEquipamento + "\n"
    "Horário: " + obterDataHora() + "\n"
    "IP: " + WiFi.localIP().toString() + "\n"
    "Wi-Fi: " + wifiNome + "\n"
    "Sinal: " + String(WiFi.RSSI()) + " dBm\n"
    "Temperatura: " + String(lerTemperaturaESP32(), 1) + " °C\n"
    "Tempo desligado: desconhecido\n"
    "Motivo do reset: " + nomeMotivoReset() + "\n"
    "Firmware: " + String(VERSAO_FIRMWARE);
}

String montarRelatorio(const String& titulo) {
  String qualidade = classificarQualidade(
    ultimaLatencia,
    jitterMedio(),
    taxaFalhas()
  );

  String mensagem =
    titulo + "\n\n"
    "Equipamento: " + nomeEquipamento + "\n"
    "Internet: " + String(internetOffline ? "OFFLINE" : "ONLINE") + "\n"
    "Qualidade: " + qualidade + " " + estrelasQualidade(qualidade) + "\n"
    "Wi-Fi: " + String(WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO") + "\n"
    "Rede: " + wifiNome + "\n";

  if (WiFi.status() == WL_CONNECTED) {
    mensagem +=
      "IP: " + WiFi.localIP().toString() + "\n"
      "Sinal: " + String(WiFi.RSSI()) + " dBm\n"
      "Canal: " + String(WiFi.channel()) + "\n";
  }

  mensagem +=
    "Servidor atual: " + ultimoServidorOK + "\n"
    "Latência atual: " + String(ultimaLatencia) + " ms\n"
    "Latência mínima: " + String(menorLatencia) + " ms\n"
    "Latência média: " + String(latenciaMedia()) + " ms\n"
    "Latência máxima: " + String(maiorLatencia) + " ms\n"
    "Jitter médio: " + String(jitterMedio()) + " ms\n"
    "Taxa de falhas: " + String(taxaFalhas(), 1) + "%\n"
    "Disponibilidade da sessão: " + String(disponibilidadeSessao(), 2) + "%\n"
    "Temperatura ESP32: " + String(lerTemperaturaESP32(), 1) + " °C\n"
    "Tempo ligado: " + formatarUptime() + "\n"
    "Quedas nesta sessão: " + String(quantidadeQuedasSessao) + "\n"
    "Quedas acumuladas: " + String(quantidadeQuedasTotal) + "\n"
    "Tempo offline acumulado: " + formatarDuracaoSegundos(totalOfflineSegundos) + "\n"
    "Última queda: " + ultimaQueda + "\n"
    "Último retorno: " + ultimoRetorno + "\n"
    "Última duração: " + ultimaDuracao + "\n"
    "Horário: " + obterDataHora();

  return mensagem;
}

String montarHistoricoTelegram(int limite = 10) {
  String mensagem = "HISTÓRICO RECENTE\n\n";

  if (historicoQuantidade == 0) {
    return mensagem + "Nenhum evento registrado.";
  }

  int inicio = historicoQuantidade - limite;
  if (inicio < 0) inicio = 0;

  for (int i = inicio; i < historicoQuantidade; i++) {
    int indice = (historicoInicio + i) % MAX_HISTORICO;
    mensagem += historico[indice] + "\n";
  }

  return mensagem;
}

// ================================================================
// TESTE DE INTERNET
// ================================================================

bool testarEndpoint(const char* url, const char* nome, unsigned long& latencia) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient cliente;
  HTTPClient http;

  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.setReuse(false);

  unsigned long inicio = millis();

  if (!http.begin(cliente, url)) return false;

  int codigo = http.GET();
  latencia = millis() - inicio;
  http.end();

  return codigo >= 200 && codigo < 500;
}

void registrarLatencia(unsigned long valor) {
  ultimaLatencia = valor;

  if (quantidadeLatencias == 0 || valor < menorLatencia) menorLatencia = valor;
  if (valor > maiorLatencia) maiorLatencia = valor;

  somaLatencias += valor;
  quantidadeLatencias++;

  if (latenciaAnterior > 0) {
    unsigned long diferenca =
      valor > latenciaAnterior
        ? valor - latenciaAnterior
        : latenciaAnterior - valor;

    somaJitter += diferenca;
    quantidadeJitter++;
  }

  latenciaAnterior = valor;
}

ResultadoInternet testarInternet() {
  ResultadoInternet resultado;
  resultado.online = false;
  resultado.servidor = "Nenhum";
  resultado.latencia = 0;
  resultado.sucessos = 0;
  resultado.falhas = 0;

  testesTotal++;

  if (WiFi.status() != WL_CONNECTED) {
    testesFalhos++;
    resultado.falhas = 3;
    return resultado;
  }

  struct Endpoint {
    const char* url;
    const char* nome;
  };

  Endpoint endpoints[] = {
    {"http://clients3.google.com/generate_204", "Google"},
    {"http://www.msftconnecttest.com/connecttest.txt", "Microsoft"},
    {"http://1.1.1.1", "Cloudflare"}
  };

  unsigned long melhorLatencia = 0;
  String melhorServidor = "Nenhum";

  for (int i = 0; i < 3; i++) {
    unsigned long latencia = 0;

    if (testarEndpoint(endpoints[i].url, endpoints[i].nome, latencia)) {
      resultado.sucessos++;

      if (melhorLatencia == 0 || latencia < melhorLatencia) {
        melhorLatencia = latencia;
        melhorServidor = endpoints[i].nome;
      }
    } else {
      resultado.falhas++;
    }

    delay(20);
  }

  if (resultado.sucessos > 0) {
    resultado.online = true;
    resultado.latencia = melhorLatencia;
    resultado.servidor = melhorServidor;
    registrarLatencia(melhorLatencia);
  } else {
    testesFalhos++;
  }

  return resultado;
}

// ================================================================
// ALERTAS INTELIGENTES
// ================================================================

void verificarAlertasInteligentes() {
  unsigned long agora = millis();

  if (
    !internetOffline &&
    ultimaLatencia >= LATENCIA_ALERTA_MS &&
    agora - ultimoAlertaLatencia >= INTERVALO_REPETIR_ALERTA
  ) {
    ultimoAlertaLatencia = agora;

    enviarTelegram(
      "⚠️ ALERTA DE INTERNET LENTA\n\n"
      "Equipamento: " + nomeEquipamento + "\n"
      "Latência atual: " + String(ultimaLatencia) + " ms\n"
      "Jitter médio: " + String(jitterMedio()) + " ms\n"
      "Qualidade: " + classificarQualidade(ultimaLatencia, jitterMedio(), taxaFalhas()) + "\n"
      "Horário: " + obterDataHora()
    );

    adicionarHistorico("ALERTA", "Internet lenta: " + String(ultimaLatencia) + " ms");
  }

  if (
    WiFi.status() == WL_CONNECTED &&
    WiFi.RSSI() <= RSSI_ALERTA_DBM &&
    agora - ultimoAlertaRSSI >= INTERVALO_REPETIR_ALERTA
  ) {
    ultimoAlertaRSSI = agora;

    enviarTelegram(
      "⚠️ ALERTA DE SINAL WI-FI FRACO\n\n"
      "Equipamento: " + nomeEquipamento + "\n"
      "Sinal atual: " + String(WiFi.RSSI()) + " dBm\n"
      "Rede: " + wifiNome + "\n"
      "Horário: " + obterDataHora()
    );

    adicionarHistorico("ALERTA", "Sinal Wi-Fi fraco: " + String(WiFi.RSSI()) + " dBm");
  }

  float temperatura = lerTemperaturaESP32();

  if (
    temperatura >= TEMPERATURA_ALERTA_C &&
    agora - ultimoAlertaTemperatura >= INTERVALO_REPETIR_ALERTA
  ) {
    ultimoAlertaTemperatura = agora;

    enviarTelegram(
      "🌡️ ALERTA DE TEMPERATURA\n\n"
      "Equipamento: " + nomeEquipamento + "\n"
      "Temperatura interna: " + String(temperatura, 1) + " °C\n"
      "Horário: " + obterDataHora()
    );

    adicionarHistorico("ALERTA", "Temperatura alta: " + String(temperatura, 1) + " °C");
  }
}

// ================================================================
// MONITORAMENTO
// ================================================================

bool conectarWiFi(unsigned long limite = 30000UL) {
  if (wifiNome.isEmpty() || wifiSenha.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("monitor-esp32");
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.begin(wifiNome.c_str(), wifiSenha.c_str());

  unsigned long inicio = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - inicio < limite) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(350);
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, HIGH);
    adicionarHistorico("WIFI", "Conectado em " + wifiNome + " | IP " + WiFi.localIP().toString());
    return true;
  }

  digitalWrite(LED_PIN, LOW);
  return false;
}

void verificarInternet() {
  if (WiFi.status() != WL_CONNECTED) {
    ultimoMotivoQueda = "Wi-Fi desconectado";

    if (millis() - ultimaTentativaWiFi >= INTERVALO_RECONEXAO_WIFI) {
      ultimaTentativaWiFi = millis();
      conectarWiFi(10000UL);
    }
  }

  ResultadoInternet resultado = testarInternet();

  if (resultado.online) {
    digitalWrite(LED_PIN, HIGH);
    falhasConsecutivas = 0;
    ultimoServidorOK = resultado.servidor;

    if (internetOffline) {
      unsigned long duracaoMs = millis() - inicioQueda;
      uint64_t duracaoSegundos = duracaoMs / 1000UL;

      internetOffline = false;
      ultimoRetorno = obterDataHora();
      ultimaDuracao = formatarDuracaoSegundos(duracaoSegundos);
      totalOfflineSegundos += duracaoSegundos;

      salvarEstatisticasPersistentes();
      adicionarHistorico("ONLINE", "Internet voltou após " + ultimaDuracao);

      enviarTelegram(
        "🟢 INTERNET RESTABELECIDA\n\n"
        "Equipamento: " + nomeEquipamento + "\n"
        "Retorno: " + ultimoRetorno + "\n"
        "Tempo offline: " + ultimaDuracao + "\n"
        "Servidor: " + resultado.servidor + "\n"
        "Latência: " + String(resultado.latencia) + " ms\n"
        "Sinal: " + String(WiFi.RSSI()) + " dBm"
      );
    }

    if (primeiraVerificacao) {
      primeiraVerificacao = false;
      adicionarHistorico("INICIO", nomeMotivoReset());
      enviarTelegram(montarMensagemInicial());
    }

    verificarAlertasInteligentes();
    return;
  }

  digitalWrite(LED_PIN, LOW);
  falhasConsecutivas++;

  ultimoMotivoQueda =
    WiFi.status() == WL_CONNECTED
      ? "Wi-Fi conectado, mas sem internet"
      : "Wi-Fi desconectado";

  if (falhasConsecutivas >= FALHAS_PARA_QUEDA && !internetOffline) {
    internetOffline = true;
    inicioQueda = millis();
    quantidadeQuedasSessao++;
    quantidadeQuedasTotal++;
    ultimaQueda = obterDataHora();

    salvarEstatisticasPersistentes();
    adicionarHistorico("OFFLINE", ultimoMotivoQueda);

    // Durante a queda não há conexão para enviar Telegram.
    // O aviso completo é enviado assim que a internet voltar.
  }

  primeiraVerificacao = false;
}

// ================================================================
// RELATÓRIO DIÁRIO
// ================================================================

void verificarRelatorioDiario() {
  if (!relatorioDiarioAtivo) return;
  if (WiFi.status() != WL_CONNECTED || internetOffline) return;

  struct tm horario;

  if (!getLocalTime(&horario, 500)) return;

  String dataHoje = obterDataISO();

  bool passouHorario =
    horario.tm_hour > relatorioHora ||
    (
      horario.tm_hour == relatorioHora &&
      horario.tm_min >= relatorioMinuto
    );

  if (
    passouHorario &&
    dataHoje.length() > 0 &&
    ultimoRelatorioData != dataHoje
  ) {
    if (enviarTelegram(montarRelatorio("📊 RELATÓRIO DIÁRIO"))) {
      salvarDataRelatorio(dataHoje);
      adicionarHistorico("RELATORIO", "Relatório diário enviado");
    }
  }
}

// ================================================================
// COMANDOS TELEGRAM
// ================================================================

void processarComandoTelegram(String texto, const String& chatRecebido) {
  if (chatRecebido != chatId) return;

  texto.trim();

  // Ignora mensagens normais. Só processa comandos.
  if (!texto.startsWith("/")) return;

  texto.toLowerCase();

  int espaco = texto.indexOf(' ');
  if (espaco > 0) texto = texto.substring(0, espaco);

  int arroba = texto.indexOf('@');
  if (arroba > 0) texto = texto.substring(0, arroba);

  if (texto == "/status") {
    enviarTelegram(montarRelatorio("📡 STATUS ATUAL"));
    return;
  }

  if (texto == "/relatorio") {
    enviarTelegram(montarRelatorio("📊 RELATÓRIO SOLICITADO"));
    return;
  }

  if (texto == "/ping") {
    ResultadoInternet resultado = testarInternet();

    if (resultado.online) {
      String qualidade = classificarQualidade(
        resultado.latencia,
        jitterMedio(),
        taxaFalhas()
      );

      enviarTelegram(
        "📶 TESTE DE QUALIDADE\n\n"
        "Resultado: ONLINE\n"
        "Servidor: " + resultado.servidor + "\n"
        "Latência: " + String(resultado.latencia) + " ms\n"
        "Jitter médio: " + String(jitterMedio()) + " ms\n"
        "Falhas acumuladas: " + String(taxaFalhas(), 1) + "%\n"
        "Qualidade: " + qualidade + " " + estrelasQualidade(qualidade) + "\n"
        "Sinal Wi-Fi: " + String(WiFi.RSSI()) + " dBm\n"
        "Temperatura: " + String(lerTemperaturaESP32(), 1) + " °C\n"
        "Horário: " + obterDataHora()
      );
    } else {
      enviarTelegram(
        "🔴 TESTE DE QUALIDADE\n\n"
        "Nenhum servidor respondeu.\n"
        "Horário: " + obterDataHora()
      );
    }

    return;
  }

  if (texto == "/historico") {
    enviarTelegram(montarHistoricoTelegram(12));
    return;
  }

  if (texto == "/painel" || texto == "/site" || texto == "/web") {
    if (WiFi.status() == WL_CONNECTED) {
      enviarTelegram(
        "🌐 PAINEL WEB DO MONITOR\n\n"
        "Equipamento: " + nomeEquipamento + "\n"
        "Endereço: http://" + WiFi.localIP().toString() + "/\n\n"
        "Abra esse endereço em um celular ou computador conectado ao mesmo Wi-Fi."
      );
    } else {
      enviarTelegram(
        "O painel não está disponível porque o ESP32 está desconectado do Wi-Fi."
      );
    }
    return;
  }

  if (texto == "/ajuda" || texto == "/start") {
    enviarTelegram(
      "COMANDOS DO MONITOR\n\n"
      "/status - estado e estatísticas\n"
      "/relatorio - relatório completo\n"
      "/ping - qualidade da internet\n"
      "/historico - últimos eventos\n"
      "/painel - envia o endereço do portal web\n"
      "/ajuda - lista de comandos"
    );
    return;
  }

  enviarTelegram("Comando não reconhecido. Use /ajuda.");
}

void consultarComandosTelegram() {
  if (WiFi.status() != WL_CONNECTED || internetOffline) return;
  if (botToken.length() < 20 || chatId.length() < 3) return;

  WiFiClientSecure cliente;
  cliente.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);

  String url =
    "https://api.telegram.org/bot" +
    botToken +
    "/getUpdates?timeout=0&limit=3&offset=" +
    String(ultimoUpdateTelegram + 1);

  if (!http.begin(cliente, url)) return;

  int codigo = http.GET();

  if (codigo < 200 || codigo >= 300) {
    http.end();
    return;
  }

  String resposta = http.getString();
  http.end();

  int posicao = 0;

  while (true) {
    int posUpdate = resposta.indexOf("\"update_id\"", posicao);
    if (posUpdate < 0) break;

    int proximoUpdate = resposta.indexOf("\"update_id\"", posUpdate + 11);
    int limite = proximoUpdate >= 0 ? proximoUpdate : resposta.length();

    String bloco = resposta.substring(posUpdate, limite);

    long updateId = extrairValorJSON(bloco, "\"update_id\"").toInt();
    String texto = extrairValorJSON(bloco, "\"text\"");

    int posChat = bloco.indexOf("\"chat\"");
    String chatRecebido = "";

    if (posChat >= 0) {
      chatRecebido = extrairValorJSON(bloco, "\"id\"", posChat);
    }

    if (updateId > ultimoUpdateTelegram) {
      ultimoUpdateTelegram = updateId;
      salvarUltimoUpdateTelegram();
    }

    if (texto.length() > 0) {
      processarComandoTelegram(texto, chatRecebido);
    }

    if (proximoUpdate < 0) break;
    posicao = proximoUpdate;
  }
}

// ================================================================
// PORTAL WEB
// ================================================================

String paginaCabecalho(const String& titulo, bool atualizar = false) {
  String pagina = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>)rawliteral";

  pagina += titulo;

  pagina += R"rawliteral(</title>
<style>
*{box-sizing:border-box}
body{
 margin:0;padding:18px;min-height:100vh;
 font-family:Arial,Helvetica,sans-serif;
 background:linear-gradient(145deg,#0b1020,#29135c);
 color:#fff
}
.container{max-width:1000px;margin:auto}
.topo{display:flex;justify-content:space-between;align-items:center;gap:14px;margin-bottom:18px}
.card{
 background:rgba(255,255,255,.09);
 border:1px solid rgba(255,255,255,.14);
 border-radius:20px;padding:22px;margin-bottom:16px;
 box-shadow:0 14px 35px rgba(0,0,0,.28)
}
.grade{display:grid;grid-template-columns:repeat(2,1fr);gap:16px}
.grade4{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
.metrica{
 background:rgba(0,0,0,.20);border-radius:14px;padding:15px;
 text-align:center
}
.metrica strong{display:block;font-size:22px;margin-bottom:6px}
.metrica span{color:#bfc3d8;font-size:13px}
.status{display:flex;justify-content:space-between;gap:15px;padding:11px 0;border-bottom:1px solid rgba(255,255,255,.09)}
.status:last-child{border-bottom:0}
.valor{text-align:right;font-weight:bold;word-break:break-word}
.online{color:#58e995}.offline{color:#ff6978}.aviso{color:#ffd166}
h1,h2{margin-top:0}
p{color:#c8cbe0;line-height:1.5}
input,select{
 width:100%;padding:13px;border-radius:11px;
 border:1px solid rgba(255,255,255,.18);
 background:rgba(0,0,0,.25);color:#fff;font-size:15px
}
label{display:block;margin:15px 0 6px;font-weight:bold}
.botao,button{
 display:inline-block;border:0;border-radius:11px;padding:13px 16px;
 margin:7px 5px 0 0;background:#7258ff;color:#fff;
 font-weight:bold;text-decoration:none;cursor:pointer
}
.secundario{background:#46516d}.perigo{background:#c53c50}
.log{
 padding:10px 0;border-bottom:1px solid rgba(255,255,255,.08);
 font-family:monospace;font-size:13px;color:#d7d9e8
}
.barra{height:11px;background:rgba(255,255,255,.12);border-radius:20px;overflow:hidden}
.barra span{display:block;height:100%;background:linear-gradient(90deg,#5ce1a1,#7b61ff)}
small{color:#aeb2c8}
@media(max-width:760px){
 body{padding:11px}
 .grade,.grade4{grid-template-columns:1fr 1fr}
 .topo{display:block}
}
@media(max-width:480px){
 .grade,.grade4{grid-template-columns:1fr}
 .status{align-items:flex-start}
 .valor{max-width:58%}
}
</style>
)rawliteral";

  if (atualizar) {
    pagina += "<script>setTimeout(function(){location.reload()},10000);</script>";
  }

  pagina += "</head><body><div class='container'>";
  return pagina;
}

String paginaRodape() {
  return "</div></body></html>";
}

void linhaStatus(String& pagina, const String& nome, const String& valor, const String& classe = "") {
  pagina += "<div class='status'><span>" + escaparHTML(nome) + "</span>";
  pagina += "<span class='valor " + classe + "'>" + escaparHTML(valor) + "</span></div>";
}

void mostrarDashboard() {
  String qualidade = classificarQualidade(ultimaLatencia, jitterMedio(), taxaFalhas());
  String classeOnline = internetOffline ? "offline" : "online";

  String pagina = paginaCabecalho("Monitor ESP32", true);

  pagina += "<div class='topo'><div><h1>" + escaparHTML(nomeEquipamento) + "</h1>";
  pagina += "<p>Monitor de internet ESP32 • Firmware " + String(VERSAO_FIRMWARE) + "</p></div>";
  pagina += "<div><a class='botao' href='/testar'>Testar agora</a></div></div>";

  pagina += "<div class='grade4'>";
  pagina += "<div class='metrica'><strong class='" + classeOnline + "'>" + String(internetOffline ? "OFFLINE" : "ONLINE") + "</strong><span>Internet</span></div>";
  pagina += "<div class='metrica'><strong>" + escaparHTML(qualidade) + "</strong><span>" + estrelasQualidade(qualidade) + "</span></div>";
  pagina += "<div class='metrica'><strong>" + String(ultimaLatencia) + " ms</strong><span>Latência</span></div>";
  pagina += "<div class='metrica'><strong>" + String(lerTemperaturaESP32(), 1) + " °C</strong><span>Temperatura</span></div>";
  pagina += "</div>";

  pagina += "<div class='grade'><div class='card'><h2>Conexão</h2>";

  linhaStatus(pagina, "Wi-Fi", WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO", WiFi.status() == WL_CONNECTED ? "online" : "offline");
  linhaStatus(pagina, "Rede", wifiNome);
  linhaStatus(pagina, "IP", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "0.0.0.0");
  linhaStatus(pagina, "Sinal", WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : "-");
  linhaStatus(pagina, "Canal", WiFi.status() == WL_CONNECTED ? String(WiFi.channel()) : "-");
  linhaStatus(pagina, "Servidor", ultimoServidorOK);
  linhaStatus(pagina, "Motivo do reset", nomeMotivoReset());

  pagina += "</div><div class='card'><h2>Qualidade</h2>";

  linhaStatus(pagina, "Latência atual", String(ultimaLatencia) + " ms");
  linhaStatus(pagina, "Latência mínima", String(menorLatencia) + " ms");
  linhaStatus(pagina, "Latência média", String(latenciaMedia()) + " ms");
  linhaStatus(pagina, "Latência máxima", String(maiorLatencia) + " ms");
  linhaStatus(pagina, "Jitter médio", String(jitterMedio()) + " ms");
  linhaStatus(pagina, "Taxa de falhas", String(taxaFalhas(), 1) + "%");
  linhaStatus(pagina, "Disponibilidade", String(disponibilidadeSessao(), 2) + "%");

  int porcentagem = (int)disponibilidadeSessao();
  if (porcentagem < 0) porcentagem = 0;
  if (porcentagem > 100) porcentagem = 100;

  pagina += "<p>Disponibilidade da sessão</p><div class='barra'><span style='width:" + String(porcentagem) + "%'></span></div>";
  pagina += "</div></div>";

  pagina += "<div class='grade'><div class='card'><h2>Estatísticas</h2>";

  linhaStatus(pagina, "Tempo ligado", formatarUptime());
  linhaStatus(pagina, "Testes executados", String(testesTotal));
  linhaStatus(pagina, "Testes sem resposta", String(testesFalhos));
  linhaStatus(pagina, "Quedas nesta sessão", String(quantidadeQuedasSessao));
  linhaStatus(pagina, "Quedas acumuladas", String(quantidadeQuedasTotal));
  linhaStatus(pagina, "Tempo offline acumulado", formatarDuracaoSegundos(totalOfflineSegundos));

  pagina += "</div><div class='card'><h2>Última interrupção</h2>";

  linhaStatus(pagina, "Última queda", ultimaQueda);
  linhaStatus(pagina, "Último retorno", ultimoRetorno);
  linhaStatus(pagina, "Duração", ultimaDuracao);
  linhaStatus(pagina, "Motivo", ultimoMotivoQueda);

  pagina += "</div></div>";

  pagina += "<div class='card'><h2>Histórico verdadeiro</h2>";

  if (historicoQuantidade == 0) {
    pagina += "<p>Nenhum evento registrado.</p>";
  } else {
    for (int i = historicoQuantidade - 1; i >= 0; i--) {
      int indice = (historicoInicio + i) % MAX_HISTORICO;
      pagina += "<div class='log'>" + escaparHTML(historico[indice]) + "</div>";
    }
  }

  pagina += "</div>";

  pagina += "<div class='card'><h2>Ações</h2>";
  pagina += "<a class='botao' href='/testar'>Executar teste</a>";
  pagina += "<a class='botao secundario' href='/enviar-relatorio'>Enviar relatório</a>";
  pagina += "<a class='botao secundario' href='/testar-telegram'>Testar Telegram</a>";
  pagina += "<a class='botao secundario' href='/configurar'>Configurações</a>";
  pagina += "<a class='botao perigo' href='/limpar-historico' onclick=\"return confirm('Limpar todo o histórico?')\">Limpar histórico</a>";
  pagina += "<a class='botao perigo' href='/reiniciar' onclick=\"return confirm('Reiniciar o ESP32?')\">Reiniciar</a>";
  pagina += "</div>";

  pagina += paginaRodape();
  servidor.send(200, "text/html; charset=utf-8", pagina);
}

void mostrarConfiguracao() {
  String pagina = paginaCabecalho("Configurar Monitor");

  pagina += "<div class='card'><h1>Configurações</h1>";
  pagina += "<form method='POST' action='/salvar'>";

  pagina += "<label>Nome do equipamento</label>";
  pagina += "<input name='nome' value='" + escaparHTML(nomeEquipamento) + "' required>";

  pagina += "<label>Nome do Wi-Fi</label>";
  pagina += "<input name='ssid' value='" + escaparHTML(wifiNome) + "' required>";

  pagina += "<label>Senha do Wi-Fi</label>";
  pagina += "<input type='password' name='senha' placeholder='Digite a senha atual' required>";

  pagina += "<label>Token do bot Telegram</label>";
  pagina += "<input name='token' value='" + escaparHTML(botToken) + "' required>";

  pagina += "<label>Chat ID</label>";
  pagina += "<input name='chatid' value='" + escaparHTML(chatId) + "' required>";

  pagina += "<label>Relatório diário</label><select name='relAtivo'>";
  pagina += String("<option value='1'") + (relatorioDiarioAtivo ? " selected" : "") + ">Ativado</option>";
  pagina += String("<option value='0'") + (!relatorioDiarioAtivo ? " selected" : "") + ">Desativado</option>";
  pagina += "</select>";

  pagina += "<label>Hora do relatório</label>";
  pagina += "<input type='number' min='0' max='23' name='relHora' value='" + String(relatorioHora) + "'>";

  pagina += "<label>Minuto do relatório</label>";
  pagina += "<input type='number' min='0' max='59' name='relMin' value='" + String(relatorioMinuto) + "'>";

  pagina += "<button type='submit'>Salvar e reiniciar</button></form>";
  pagina += "<small>Modo de configuração: Wi-Fi MonitorESP32, senha 12345678, endereço 192.168.4.1</small>";
  pagina += "</div>" + paginaRodape();

  servidor.send(200, "text/html; charset=utf-8", pagina);
}

void salvarFormulario() {
  nomeEquipamento = servidor.arg("nome");
  wifiNome = servidor.arg("ssid");
  wifiSenha = servidor.arg("senha");
  botToken = servidor.arg("token");
  chatId = servidor.arg("chatid");
  relatorioDiarioAtivo = servidor.arg("relAtivo") == "1";
  relatorioHora = servidor.arg("relHora").toInt();
  relatorioMinuto = servidor.arg("relMin").toInt();

  nomeEquipamento.trim();
  wifiNome.trim();
  botToken.trim();
  chatId.trim();

  if (relatorioHora < 0 || relatorioHora > 23) relatorioHora = 20;
  if (relatorioMinuto < 0 || relatorioMinuto > 59) relatorioMinuto = 0;

  salvarConfiguracoes();

  servidor.send(200, "text/html; charset=utf-8",
    paginaCabecalho("Salvo") +
    "<div class='card'><h1>Configuração salva</h1><p>O ESP32 será reiniciado.</p></div>" +
    paginaRodape()
  );

  delay(1800);
  ESP.restart();
}

void configurarRotas() {
  servidor.on("/", HTTP_GET, []() {
    if (modoConfiguracao) mostrarConfiguracao();
    else mostrarDashboard();
  });

  servidor.on("/configurar", HTTP_GET, mostrarConfiguracao);
  servidor.on("/salvar", HTTP_POST, salvarFormulario);

  servidor.on("/testar", HTTP_GET, []() {
    if (!modoConfiguracao) verificarInternet();
    servidor.sendHeader("Location", "/");
    servidor.send(302);
  });

  servidor.on("/enviar-relatorio", HTTP_GET, []() {
    if (!modoConfiguracao) {
      enviarTelegram(montarRelatorio("📊 RELATÓRIO SOLICITADO PELO PAINEL"));
      adicionarHistorico("RELATORIO", "Relatório enviado pelo painel");
    }

    servidor.sendHeader("Location", "/");
    servidor.send(302);
  });

  servidor.on("/testar-telegram", HTTP_GET, []() {
    if (!modoConfiguracao) {
      enviarTelegram(
        "✅ TESTE DO TELEGRAM\n\n"
        "Equipamento: " + nomeEquipamento + "\n"
        "Comunicação funcionando.\n"
        "Horário: " + obterDataHora()
      );
    }

    servidor.sendHeader("Location", "/");
    servidor.send(302);
  });

  servidor.on("/limpar-historico", HTTP_GET, []() {
    historicoInicio = 0;
    historicoQuantidade = 0;

    for (int i = 0; i < MAX_HISTORICO; i++) historico[i] = "";

    salvarHistoricoCompleto();

    servidor.sendHeader("Location", "/");
    servidor.send(302);
  });

  servidor.on("/reiniciar", HTTP_GET, []() {
    servidor.send(200, "text/plain; charset=utf-8", "Reiniciando...");
    delay(700);
    ESP.restart();
  });

  servidor.onNotFound([]() {
    if (modoConfiguracao) mostrarConfiguracao();
    else {
      servidor.sendHeader("Location", "/");
      servidor.send(302);
    }
  });

  servidor.begin();
}

// ================================================================
// MODO CONFIGURAÇÃO E OTA
// ================================================================

void iniciarModoConfiguracao() {
  modoConfiguracao = true;

  WiFi.disconnect(true);
  delay(400);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NOME, AP_SENHA);

  dns.start(53, "*", WiFi.softAPIP());
  configurarRotas();

  Serial.println("Modo de configuração");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
}

void configurarOTA() {
  ArduinoOTA.setHostname("monitor-esp32");
  ArduinoOTA.setPassword("esp32admin");

  ArduinoOTA.onStart([]() {
    adicionarHistorico("OTA", "Atualização iniciada");
  });

  ArduinoOTA.onEnd([]() {
    adicionarHistorico("OTA", "Atualização concluída");
  });

  ArduinoOTA.onError([](ota_error_t erro) {
    adicionarHistorico("OTA", "Erro " + String(erro));
  });

  ArduinoOTA.begin();
}

// ================================================================
// SETUP E LOOP
// ================================================================

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  delay(1200);

  carregarConfiguracoes();

  bool configurado =
    !wifiNome.isEmpty() &&
    !wifiSenha.isEmpty() &&
    !botToken.isEmpty() &&
    !chatId.isEmpty();

  if (!configurado) {
    iniciarModoConfiguracao();
    return;
  }

  if (!conectarWiFi()) {
    iniciarModoConfiguracao();
    return;
  }

  modoConfiguracao = false;

  // Horário de Brasília
  configTime(
    -3 * 3600,
    0,
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com"
  );

  delay(1200);

  configurarRotas();
  configurarOTA();

  verificarInternet();

  ultimoTeste = millis();
  ultimaConsultaBot = millis();
}

void loop() {
  servidor.handleClient();

  if (modoConfiguracao) {
    dns.processNextRequest();

    if (millis() - ultimaPiscada >= 500) {
      ultimaPiscada = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }

    delay(5);
    return;
  }

  ArduinoOTA.handle();

  if (millis() - ultimoTeste >= INTERVALO_TESTE) {
    ultimoTeste = millis();
    verificarInternet();
  }

  if (millis() - ultimaConsultaBot >= INTERVALO_BOT) {
    ultimaConsultaBot = millis();
    consultarComandosTelegram();
  }

  verificarRelatorioDiario();

  if (internetOffline) {
    if (millis() - ultimaPiscada >= 500) {
      ultimaPiscada = millis();
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  delay(5);
}
