/*
  Firmware final - Caixa d'água (SR04 + DHT22)
  - Leitura a cada 5 minutos (300000 ms) com agendamento preciso
  - Armazena leituras como JSON em LinkedList<String> (máx 1000)
  - NTP para timestamps reais (UTC-3)
  - Endpoints:
      GET /ultima     -> último JSON
      GET /historico  -> array JSON com todas as leituras
  - Envia alerta Telegram se volume menor que 1000 litros
  - Bibliotecas necessárias:
      LinkedList (Ivan Seidel)
      DHT sensor library (Adafruit)
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <LinkedList.h>
#include <time.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>

// ------------------ CONFIG ------------------
//const char* WIFI_SSID = "Sitio M&M";
//const char* WIFI_PASS = "ranchofeliz";

//const char* WIFI_SSID = "ZTE_DA98";
//const char* WIFI_PASS = "2tDusEH5Au";

struct AppConfig {
  // se envia alarme ao telegram
  bool telegramHabilitado;
  //token do telegram
  String telegramBotToken;
  //id do bot no telegram
  String telegramChatId;
  //valor para aviso de volume da caixa (litros)
  int volumeAviso;
  //valor para aviso crítico de volume da caixa (litros)
  int volumeCritico;
  //alarme de temperatura alta no sistema (graus celcius)
  int temperaturaAviso;
  //alarme de temperatura criticamente alta no sistema (graus celcius)
  int temperaturaCritica;
  //se possui repositorio a ser alimentado no firebase
  bool firebaseHabilitado;
  //URL do repositório no firebase
  String databaseUrl;
  //chave da api no firebase
  String apiKey;
  //intervalo
  unsigned long intervaloLeitura;

  // altura da caixa de água (centímetros)
  int alturaCaixa;  // cm
  //tipo da caixa de água -> 0 = redonda | 1 = retangular
  int tipoCaixa;

  // diâmetro de uma caixa redonda (centímetros)
  int diametroCaixa;

  // largura de uma caixa retangular (centímetros)
  int larguraCaixa;  // cm
    // comprimento de uma caixa retangular (centímetros)
  int comprimentoCaixa;  // cm
};

AppConfig config;

enum Criticidade {
  LIMPO,
  AVISO,
  ALARME,
};

struct Alarme {
  Criticidade volume;
  Criticidade temperatura;
};

Alarme ultimoAlarme = { LIMPO, LIMPO };

bool carregarConfig() {
  if (!LittleFS.exists("/config.json")) {
    Serial.println("Config nao existe, usando padrao");
    return false;
  }

  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;

  String json = f.readString();
  f.close();

  DynamicJsonDocument doc(512);
  deserializeJson(doc, json);
  config.telegramHabilitado = doc["telegramHabilitado"] | false;
  config.firebaseHabilitado = doc["firebaseHabilitado"] | false;
  config.telegramBotToken = doc["telegramBotToken"] | "";
  config.telegramChatId = doc["telegramChatId"] | "";

  config.volumeAviso = doc["volumeAviso"] | 1500;
  config.volumeCritico = doc["volumeCritico"] | 1000;
  config.temperaturaAviso = doc["temperaturaAviso"] | 50;
  config.temperaturaCritica = doc["temperaturaCritica"] | 70;

  config.databaseUrl = doc["databaseUrl"] | "";
  config.apiKey = doc["apiKey"] | "";
  config.intervaloLeitura = doc["intervaloLeitura"] | 300;

  config.alturaCaixa = doc["alturaCaixa"] | 100;
  config.tipoCaixa = doc["tipoCaixa"] | 0;
  config.diametroCaixa = doc["diametroCaixa"] | 50;
  config.larguraCaixa = doc["larguraCaixa"] | 100;
  config.comprimentoCaixa = doc["comprimentoCaixa"] | 100;


  Serial.println("Config carregada");
  return true;
}

void salvarConfig() {
  DynamicJsonDocument doc(512);

  doc["telegramHabilitado"] = config.telegramHabilitado;
  doc["firebaseHabilitado"] = config.firebaseHabilitado;
  doc["telegramBotToken"] = config.telegramBotToken;
  doc["telegramChatId"] = config.telegramChatId;

  doc["volumeAviso"] = config.volumeAviso;
  doc["volumeCritico"] = config.volumeCritico;
  doc["temperaturaAviso"] = config.temperaturaAviso;
  doc["temperaturaCritica"] = config.temperaturaCritica;

  doc["databaseUrl"] = config.databaseUrl;
  doc["apiKey"] = config.apiKey;
  doc["intervaloLeitura"] = config.intervaloLeitura;

  doc["alturaCaixa"] = config.alturaCaixa;
  doc["tipoCaixa"] = config.tipoCaixa;
  doc["diametroCaixa"] = config.diametroCaixa;
  doc["larguraCaixa"] = config.larguraCaixa;
  doc["comprimentoCaixa"] = config.comprimentoCaixa;


  File f = LittleFS.open("/config.json", "w");
  serializeJson(doc, f);
  f.close();

  Serial.println("Config salva");
}


//const char* TELEGRAM_BOT_TOKEN = "8324004011:AAFIn_tryjr7QMVRXlbgwqTikvvHF--VhuE";
//const char* TELEGRAM_CHAT_ID = "8283613876";

const int MAX_HISTORICO = 10;

const unsigned long NTP_TIMEOUT_MS = 15000;  // ms
const long GMT_OFFSET_SEC = -3 * 3600;       // UTC-3
const int DAYLIGHT_OFFSET_SEC = 0;

// ------------------ PINOUT (GPIO numbers) ------------------
#define TRIG_PIN 5  // D1 -> GPIO5
#define ECHO_PIN 4  // D2 -> GPIO4
#define DHTPIN 14   // D5 -> GPIO14

// ------------------ SENSORES ------------------
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ------------------ SERVER / TELEGRAM / HISTÓRICO ------------------
ESP8266WebServer server(80);
WiFiClientSecure telegramClient;
LinkedList<String> historico = LinkedList<String>();

// ------------------ UTIL: url-encode ------------------
String urlEncode(const String& str) {
  String encoded = "";
  char c;
  const char* hex = "0123456789ABCDEF";
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      encoded += hex[(c >> 4) & 0xF];
      encoded += hex[c & 0xF];
    }
  }
  return encoded;
}

// ------------------ UTIL: get timestamp ISO ------------------
String getTimestampISO() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}


// ------------------ SR04: leitura distância (cm) ------------------
long lerDistancia() {

  const int N = 20;
  long leituras[N];
  int validas = 0;

  for (int i = 0; i < N; i++) {

    // Trigger do SR04
    //    digitalWrite(TRIG_PIN, LOW);
    //    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(50);
    digitalWrite(TRIG_PIN, LOW);

    // Captura
    long duracao = pulseIn(ECHO_PIN, HIGH, 20);

    // Guarda apenas válidas
    if (duracao > 0) {
      leituras[validas] = (duracao * 0.0343) / 2.0;  // converte para cm
      validas++;
    }
    Serial.println("leitura " + String(validas - 1) + " duracao " + String(duracao));
    delay(100);  // pequeno intervalo entre leituras
  }

  // Nenhuma leitura válida
  if (validas == 0) return -1;

  // Apenas 1 leitura válida → retorna ela
  if (validas == 1) return leituras[0];

  // Ordena as leituras válidas
  for (int i = 0; i < validas - 1; i++) {
    for (int j = i + 1; j < validas; j++) {
      if (leituras[j] < leituras[i]) {
        long t = leituras[i];
        leituras[i] = leituras[j];
        leituras[j] = t;
      }
    }
  }

  // Se >= 3 válidas: descarta menor e maior → média robusta
  long soma = 0;
  int inicio = 0;
  int fim = validas;

  if (validas >= 3) {
    inicio = 1;         // descarta menor
    fim = validas - 1;  // descarta maior
  }

  for (int i = inicio; i < fim; i++) {
    soma += leituras[i];
  }

  int divisor = fim - inicio;
  Serial.println("distancia medida " + String(soma / divisor));
  return soma / divisor;
}

// Dados da caixa
//float ALTURA_CAIXA = 150.0;  // cm
//float RAIO_CAIXA = 112.5;    // cm
//const float AREA_BASE_CIRCULAR = 3.14159 * RAIO_CAIXA * RAIO_CAIXA;  // cm²
//float LARGURA_CAIXA = 2.0;
//float COMPRIMENTO_CAIXA = 2.0;
//const float AREA_BASE_RETANGULAR =  LARGURA_CAIXA  * COMPRIMENTO_CAIXA;  // cm²

//int TIPO_CAIXA = 0;  //0 - caixa redonda, 1- caixa retangular

float calcularVolumeLitros(float distancia) {
  float raio = config.diametroCaixa / 2;
  float alturaAgua = config.alturaCaixa - distancia;
  if (alturaAgua < 0) alturaAgua = 0;
  if (alturaAgua > config.alturaCaixa) alturaAgua = config.alturaCaixa;
  float volumeLitros;
  float volumeCm3;
  if (config.tipoCaixa == 0) {
    volumeCm3 = 3.14159 * raio * raio * alturaAgua;
  } else {
    volumeCm3 = config.larguraCaixa * config.comprimentoCaixa * alturaAgua;
  }
  volumeLitros = volumeCm3 / 1000.0;
  return volumeLitros;
}

// ------------------ Adiciona JSON ao histórico (LinkedList) ------------------
void adicionaHistoricoJson(const String& json) {
  historico.add(json);
  if (historico.size() > MAX_HISTORICO) {
    historico.shift();  // remove o mais antigo
  }
}

// ------------------ Enviar Telegram ------------------
void enviarTelegram(const String& texto) {
  if (WiFi.status() != WL_CONNECTED) return;

  telegramClient.setInsecure();
  if (!telegramClient.connect("api.telegram.org", 443)) {
    Serial.println("Falha ao conectar Telegram");
    return;
  }

  String msgEncoded = urlEncode(texto);
  String request = String("GET /bot") + config.telegramBotToken + "/sendMessage?chat_id=" + config.telegramChatId + "&text=" + msgEncoded + " HTTP/1.1\r\n";
  request += "Host: api.telegram.org\r\n";
  request += "Connection: close\r\n\r\n";

  telegramClient.print(request);

  // ler resposta curta (opcional)
  unsigned long start = millis();
  while (telegramClient.connected() && millis() - start < 2000) {
    while (telegramClient.available()) {
      String line = telegramClient.readStringUntil('\n');
      (void)line;  // descartamos
    }
  }
  telegramClient.stop();
}

// ------------------ Endpoint /ultima ------------------
void handleUltima() {
  if (historico.size() == 0) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{}");
    return;
  }
  String last = historico.get(historico.size() - 1);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", last);
}

// ------------------ Endpoint /historico ------------------
void handleHistorico() {
  String out = "[";
  for (int i = 0; i < historico.size(); i++) {
    out += historico.get(i);
    if (i < historico.size() - 1) out += ",";
  }
  out += "]";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

void handleApp() {
  String out = R"====(
<!DOCTYPE html>
<html lang="pt-br">

<head>
  <meta charset="UTF-8" />
  <title>Capituva Monitor</title>
  <style>
    body {
      font-family: Arial;
      margin: 20px;
      background: #f0f2f5;
    }

    h1 {
      text-align: center;
    }

    .card {
      background: white;
      padding: 15px;
      margin-bottom: 20px;
      border-radius: 10px;
      box-shadow: 0 2px 6px rgba(0, 0, 0, 0.15);
    }

    canvas {
      width: 100% !important;
      max-width: 700px;
    }

    #config-content {
      transition: max-height 0.3s ease, opacity 0.3s ease;
      overflow: hidden;
    }

    .disabled {
      opacity: 0.5;
      pointer-events: none;
    }
  </style>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>

<body>
  <h1>Capituva Monitor</h1>
  <div class="card">
    <h3 style="cursor:pointer" onclick="toggleConfig()">
      ⚙️ Configurações
    </h3>
    <div id="config-content">

      <label>Intervalo (segundos)</label><br>
      <input id="intervalo" type="number"><br><br>

      <label>
        <input type="checkbox" id="enableTelegram" onchange="toggleTelegram()">
        Habilitar Telegram
      </label><br><br>

      <div id="telegram-config">
        <label>Telegram Bot Token</label><br>
        <input id="bot" style="width:100%"><br><br>

        <label>Telegram Chat ID</label><br>
        <input id="chat" style="width:100%"><br><br>

        <label>aviso mediano de volume</label><br>
        <input id="volumeAviso" style="width:100%"><br><br>

        <label>aviso crítico de volume</label><br>
        <input id="volumeCritico" style="width:100%"><br><br>

      </div>

      <label>
        <input type="checkbox" id="enableFirebase" onchange="toggleFirebase()">
        Habilitar Firebase
      </label><br><br>

      <div id="firebase-config">
        <label>Firebase URL</label><br>
        <input id="db" style="width:100%"><br><br>

        <label>API Key</label><br>
        <input id="api" style="width:100%"><br><br>
      </div>

      <div>
        <label>Altura da Caixa (cm)</label><br>
        <input id="altura" type="number"><br><br>

        <label>Tipo da Caixa</label><br>
        <select id="tipo" onchange="atualizarTipo()">
          <option value="0">Redonda</option>
          <option value="1">Retangular</option>
        </select><br><br>

        <div id="caixa-redonda">
          <label>Diâmetro da Caixa (cm)</label><br>
          <input id="diametro" type="number"><br><br>
        </div>

        <div id="caixa-retangular" style="display:none">
          <label>Largura (cm)</label><br>
          <input id="largura" type="number"><br><br>

          <label>Comprimento (cm)</label><br>
          <input id="comprimento" type="number"><br><br>
        </div>
      </div>
      <button onclick="salvarConfig()">Salvar</button>
    </div>
  </div>

  <div class="card">
    <h3>Última leitura</h3>
    <div id="ultima">Carregando...</div>
  </div>

  <div class="card">
    <h3>Histórico de volume</h3>
    <canvas id="grafDist"></canvas>
  </div>

  <div class="card">
    <h3>Histórico de Temperatura</h3>
    <canvas id="grafTemp"></canvas>
  </div>

  <div class="card">
    <h3>Histórico de Umidade</h3>
    <canvas id="grafUmid"></canvas>
  </div>
  <script>

    function toggleTelegram() {
      const enabled = enableTelegram.checked;
      document.getElementById("telegram-config")
        .classList.toggle("disabled", !enabled);
    }

    function toggleFirebase() {
      const enabled = enableFirebase.checked;
      document.getElementById("firebase-config")
        .classList.toggle("disabled", !enabled);
    }



    async function carregarUltima() {
      const r = await fetch('/api/ultima');
      const j = await r.json();

      document.getElementById("ultima").innerHTML =
        "Volume: " + j.volume + " l<br>" +
        "Temp interna: " + j.tempInterna + " °C<br>" +
        "Umidade interna: " + j.umidadeInterna + " %<br>" +
        "Temp externa: " + j.tempExterna + " °C<br>" +
        "Timestamp: " + new Date(j.timestamp).toLocaleString('pt-BR');
    }

    let distChart, tempChart, umidChart;

    async function carregarHistorico() {
      const r = await fetch('/api/historico');
      const dados = await r.json();

      const labels = dados.map(x => new Date(x.timestamp).toLocaleTimeString('pt-BR'));
      const dist = dados.map(x => x.volume);
      const temp = dados.map(x => x.tempInterna);
      const umid = dados.map(x => x.umidadeInterna);

      if (distChart) distChart.destroy();
      if (tempChart) tempChart.destroy();
      if (umidChart) umidChart.destroy();

      distChart = new Chart(document.getElementById('grafDist'), {
        type: 'line',
        data: {
          labels: labels,
          datasets: [{
            label: 'Volume (l)',
            data: dist,
            borderWidth: 2
          }]
        }
      });

      tempChart = new Chart(document.getElementById('grafTemp'), {
        type: 'line',
        data: {
          labels: labels,
          datasets: [{
            label: 'Temperatura (°C)',
            data: temp,
            borderWidth: 2
          }]
        }
      });

      umidChart = new Chart(document.getElementById('grafUmid'), {
        type: 'line',
        data: {
          labels: labels,
          datasets: [{
            label: 'Umidade (%)',
            data: umid,
            borderWidth: 2
          }]
        }
      });
    }

    async function atualizarTudo() {
      await carregarUltima();
      await carregarHistorico();
    }

    atualizarTudo();

    let configAberto = true;

    function toggleConfig() {
      const c = document.getElementById("config-content");

      if (configAberto) {
        c.style.maxHeight = "0";
        c.style.opacity = "0";
      } else {
        c.style.maxHeight = "2000px"; // valor grande
        c.style.opacity = "1";
      }

      configAberto = !configAberto;
    }

    toggleConfig();



    function atualizarTipo() {
      const caixaRedonda = document.getElementById("caixa-redonda");
      const caixaRetangular = document.getElementById("caixa-retangular");

      if (tipo.value == "0") {
        caixaRedonda.style.display = "block";
        caixaRetangular.style.display = "none";
      } else {
        caixaRedonda.style.display = "none";
        caixaRetangular.style.display = "block";
      }
    }


    async function carregarConfig() {
      const r = await fetch('/api/config');
      const c = await r.json();

      bot.value = c.telegramBotToken;
      chat.value = c.telegramChatId;

      volumeAviso.value = c.volumeAviso;
      volumeCritico.value = c.volumeCritico;

      db.value = c.databaseUrl;
      api.value = c.apiKey;
      intervalo.value = c.intervaloLeitura;
      altura.value = c.alturaCaixa;
      tipo.value = c.tipoCaixa;

      diametro.value = c.diametroCaixa;
      largura.value = c.larguraCaixa;
      comprimento.value = c.comprimentoCaixa;

      enableTelegram.checked = c.telegramHabilitado;
      enableFirebase.checked = c.firebaseHabilitado;

      toggleTelegram();
      toggleFirebase();

      atualizarTipo();

    }

    async function salvarConfig() {
      await fetch('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          telegramHabilitado: enableTelegram.checked,
          firebaseHabilitado: enableFirebase.checked,

          telegramBotToken: bot.value,
          telegramChatId: chat.value,
          volumeAviso: volumeAviso.value,
          volumeCritico: volumeCritico.value,
          databaseUrl: db.value,
          apiKey: api.value,

          intervaloLeitura: Number(intervalo.value),
          alturaCaixa: Number(altura.value),
          tipoCaixa: Number(tipo.value),
          diametroCaixa: Number(diametro.value),
          larguraCaixa: Number(largura.value),
          comprimentoCaixa: Number(comprimento.value),
        })

      });
      alert("Configuração salva! Reinicie o ESP.");
    }



    carregarConfig();



    // AUTO-REFRESH a cada 1 minuto (60s)
    setInterval(() => {
      atualizarTudo();
    }, 60000);


  </script>
</body>

</html>
)====";


  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/html", out);
}

// ------------------ Setup NTP ------------------

// Variáveis para manter o relógio estável
unsigned long millisNTP = 0;
unsigned long epochNTP = 0;  // segundos

// Obtém o tempo “contínuo”
unsigned long getStableEpoch() {
  unsigned long agora = millis();
  return epochNTP + (agora - millisNTP) / 1000;
}

void syncNTP() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // Salva o epoch atual
    epochNTP = mktime(&timeinfo);

    // Marca o millis() do momento da captura
    millisNTP = millis();

    Serial.printf("NTP sincronizado! Epoch atual: %lu\n", epochNTP);
  } else {
    Serial.println("Falha ao sincronizar NTP.");
  }
}


void setupNTP() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  unsigned long start = millis();
  Serial.print("Aguardando NTP");
  while (time(nullptr) < 1000000000) {
    Serial.print(".");
    delay(500);
    if (millis() - start > NTP_TIMEOUT_MS) {
      Serial.println("\nTimeout NTP (continuando com millis())");
      return;
    }
  }
  syncNTP();
  Serial.println("\nNTP sincronizado: " + getTimestampISO());
}

// ------------------ FIREBASE ------------------
//const char* API_KEY = "AIzaSyAlRIiLVigM39ELJ8bXJUIrcIzrviCAHFg";
char* DATABASE_URL;

WiFiClientSecure firebaseClient;


void enviarFirebase(const String& json) {
  DATABASE_URL = (char*)config.databaseUrl.c_str();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase: sem WiFi");
    return;
  }

  firebaseClient.setInsecure();  // ignora certificado SSL

  String path = String("/capituva_monitor/historico.json?auth=") + config.apiKey;

  if (!firebaseClient.connect("automacaositiommcapituva-default-rtdb.firebaseio.com", 443)) {
    Serial.println("Erro conectar Firebase");
    return;
  }

  String request =
    "POST " + path + " HTTP/1.1\r\n"
                     "Host: automacaositiommcapituva-default-rtdb.firebaseio.com\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: "
    + String(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n"
    + json;

  firebaseClient.print(request);

  // ler resposta (opcional)
  unsigned long start = millis();
  while (firebaseClient.connected() && millis() - start < 2000) {
    while (firebaseClient.available()) {
      String line = firebaseClient.readStringUntil('\n');
      Serial.println("Firebase -> " + line);
    }
  }

  firebaseClient.stop();
  Serial.println("Firebase: enviado");
}

void salvarUltimaLeitura(unsigned long valor) {
  File f = LittleFS.open("/ultima.dat", "w");
  if (!f) {
    Serial.println("ERRO ao abrir /ultima.dat para escrita!");
    return;
  }

  f.write((uint8_t*)&valor, sizeof(valor));  // grava binário
  f.close();

  Serial.printf("Salvou ultimaLeitura=%lu no LittleFS\n", valor);
}

bool carregarUltimaLeitura(unsigned long& valorOut) {
  if (!LittleFS.exists("/ultima.dat")) {
    Serial.println("Arquivo /ultima.dat nao existe. Usando valor padrao.");
    return false;
  }

  File f = LittleFS.open("/ultima.dat", "r");
  if (!f) {
    Serial.println("ERRO ao abrir /ultima.dat para leitura!");
    return false;
  }

  if (f.size() != sizeof(unsigned long)) {
    Serial.println("Tamanho inesperado do arquivo! Ignorando.");
    f.close();
    return false;
  }

  f.read((uint8_t*)&valorOut, sizeof(valorOut));
  f.close();

  Serial.printf("Carregado ultimaLeitura=%lu do LittleFS\n", valorOut);
  return true;
}

// funcao para retries
const int DHT_READ_RETRIES = 5;
const int DHT_READ_RETRY_DELAY_MS = 200;

bool lerDHTComRetries(float& outTemp, float& outHum) {
  outTemp = NAN;
  outHum = NAN;

  for (int i = 0; i < DHT_READ_RETRIES; ++i) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    Serial.printf("DHT tentativa %d: temp=%s  hum=%s\n", i + 1,
                  isnan(t) ? "NAN" : String(t).c_str(),
                  isnan(h) ? "NAN" : String(h).c_str());

    if (!isnan(t) && !isnan(h)) {
      outTemp = t;
      outHum = h;
      return true;
    }
    delay(DHT_READ_RETRY_DELAY_MS);
  }
  return false;
}

void conectarWiFi() {
  WiFiManager wm;

  // Nome do AP de configuração
  wm.setConfigPortalTimeout(180);  // 3 minutos

  bool res = wm.autoConnect("Monitor-Setup");

  if (!res) {
    Serial.println("Falha ao conectar WiFi");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi conectado com sucesso!");
  Serial.println(WiFi.localIP());
}

void verificaAlarme(float volume, float temperatura, float umidade, String ts) {
  // Alerta Telegram
  String alerta = "";
  if (volume < config.volumeCritico) {
    if (ultimoAlarme.volume != Criticidade::ALARME) {
      alerta = "⚠ ALERTA: Volume de água muito baixo, caixa vazia!\n";
      alerta += "Volume: " + String(volume) + " l";
      ultimoAlarme.volume = Criticidade::ALARME;
    }
  } else if (volume < config.volumeAviso) {
    if (ultimoAlarme.volume != Criticidade::AVISO) {
      alerta = "⚠ ALERTA: Volume de água baixo!\n";
      alerta += "Volume: " + String(volume) + " l";
      ultimoAlarme.volume = Criticidade::AVISO;
    }
  } else {
    if (ultimoAlarme.volume != Criticidade::LIMPO) {
      alerta = "⚠ ALERTA: Volume de água voltou ao normal!\n";
      alerta += "Volume: " + String(volume) + " l";
      ultimoAlarme.volume = Criticidade::LIMPO;
    }
  }

  if (!alerta.isEmpty()) {
    if (!isnan(temperatura)) alerta += "Temp Int: " + String(temperatura) + " C\n";
    if (!isnan(umidade)) alerta += "Umid Int: " + String(umidade) + " %\n";
    alerta += "TS: " + ts;
    if (config.telegramHabilitado) {
      enviarTelegram(alerta);
      Serial.println("Alerta Telegram enviado.");
    } else Serial.println("Telegram desabilitado");
  } else Serial.println("Sem alteração de status.");
}

unsigned long ultimaLeitura = -300;
const unsigned long intervaloLeitura = 300UL;  // 5 minutos em ms


// ------------------ setup() ------------------
void setup() {
  Serial.begin(115200);
  delay(10);
  Serial.println("Inicio do setup.");
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  dht.begin();

  // WiFi com portal de configuração
  conectarWiFi();
  setupNTP();


  digitalWrite(TRIG_PIN, LOW);

//ArduinoOTA.setPassword("321");
 ArduinoOTA.begin();


  Serial.println("\nIniciando LittleFS...");
  if (!LittleFS.begin()) {
    Serial.println("LittleFS falhou!");
    while (true)
      ;
  }
  Serial.println("LittleFS OK.");
  if (!carregarConfig()) {
    config.telegramBotToken = "8324004011:AAFIn_tryjr7QMVRXlbgwqTikvvHF--VhuE";
    config.telegramChatId = "8283613876";
    config.databaseUrl = "https://automacaositiommcapituva-default-rtdb.firebaseio.com";
    config.apiKey = "AIzaSyAlRIiLVigM39ELJ8bXJUIrcIzrviCAHFg";
    config.intervaloLeitura = 300;
    config.volumeAviso = 1500;
    config.volumeCritico = 1000;
    config.temperaturaAviso = 50;
    config.temperaturaCritica = 70;
    config.alturaCaixa = 150;
    config.tipoCaixa = 0;
    config.diametroCaixa = 150;
    config.larguraCaixa = 100;
    config.comprimentoCaixa = 150;
    config.telegramHabilitado = false;
    config.firebaseHabilitado = false;
    salvarConfig();
  }

  if (!carregarUltimaLeitura(ultimaLeitura)) {
    ultimaLeitura = ultimaLeitura = -300;  // se não existe, usa padrão
  }

  Serial.printf("Valor inicial de ultimaLeitura = %lu\n", ultimaLeitura);
  // HTTP endpoints
  server.on("/", handleApp);
  server.on("/api/ultima", handleUltima);
  server.on("/api/historico", handleHistorico);

  server.on("/api/config", HTTP_GET, []() {
    DynamicJsonDocument doc(512);
    doc["telegramHabilitado"] = config.telegramHabilitado;
    doc["firebaseHabilitado"] = config.firebaseHabilitado;
    doc["telegramBotToken"] = config.telegramBotToken;
    doc["telegramChatId"] = config.telegramChatId;

    doc["volumeAviso"] = config.volumeAviso;
    doc["volumeCritico"] = config.volumeCritico;
    doc["temperaturaAviso"] = config.temperaturaAviso;
    doc["temperaturaCritica"] = config.temperaturaCritica;

    doc["databaseUrl"] = config.databaseUrl;
    doc["apiKey"] = config.apiKey;
    doc["intervaloLeitura"] = config.intervaloLeitura;
    doc["alturaCaixa"] = config.alturaCaixa;
    doc["tipoCaixa"] = config.tipoCaixa;
    doc["diametroCaixa"] = config.diametroCaixa;
    doc["larguraCaixa"] = config.larguraCaixa;
    doc["comprimentoCaixa"] = config.comprimentoCaixa;

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.on("/api/config", HTTP_POST, []() {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, server.arg("plain"));
    config.telegramHabilitado = doc["telegramHabilitado"] | config.telegramHabilitado;
    config.firebaseHabilitado = doc["firebaseHabilitado"] | config.firebaseHabilitado;
    config.telegramBotToken = doc["telegramBotToken"] | config.telegramBotToken;
    config.telegramChatId = doc["telegramChatId"] | config.telegramChatId;

    config.volumeAviso = doc["volumeAviso"] | config.volumeAviso;
    config.volumeCritico = doc["volumeCritico"] | config.volumeCritico;
    config.temperaturaAviso = doc["temperaturaAviso"] | config.temperaturaAviso;
    config.temperaturaCritica = doc["temperaturaCritica"] | config.temperaturaCritica;

    config.databaseUrl = doc["databaseUrl"] | config.databaseUrl;
    config.apiKey = doc["apiKey"] | config.apiKey;
    config.intervaloLeitura = doc["intervaloLeitura"] | config.intervaloLeitura;
    config.alturaCaixa = doc["alturaCaixa"] | 150;
    config.tipoCaixa = doc["tipoCaixa"] | 0;
    config.diametroCaixa = doc["diametroCaixa"] | 50;
    config.larguraCaixa = doc["larguraCaixa"] | 100;
    config.comprimentoCaixa = doc["comprimentoCaixa"] | 100;

    salvarConfig();
    server.send(200, "text/plain", "OK");
  });


  server.begin();
  Serial.println("HTTP server iniciado na porta 80");
}

// ------------------ loop() ------------------



void loop() {


  server.handleClient();
  ArduinoOTA.handle();
  unsigned long agora = getStableEpoch();
  delay(200);


  if ((long)(agora - ultimaLeitura) >= (long)config.intervaloLeitura) {
    ultimaLeitura = agora;

    // -------- LEITURA DOS SENSORES --------
    float volume = calcularVolumeLitros(lerDistancia());  // cm->l (-1 se timeout) nao tratado

    float tempInt = NAN;
    float umidInt = NAN;
    bool okDHT = lerDHTComRetries(tempInt, umidInt);
    if (!okDHT) {
      Serial.println("ERRO: leitura DHT falhou apos " + String(DHT_READ_RETRIES) + " tentativas.");
    }

    String ts = (time(nullptr) >= 1000000000) ? getTimestampISO() : String(millis());

    // Monta JSON
    String json = "{";
    json += "\"timestamp\":\"" + ts + "\",";
    json += "\"volume\":" + String(volume) + ",";
    json += "\"tempInterna\":" + (isnan(tempInt) ? "null" : String(tempInt)) + ",";
    json += "\"umidadeInterna\":" + (isnan(umidInt) ? "null" : String(umidInt)) + ",";
    json += "\"tempExterna\":null,";
    json += "\"umidadeExterna\":null";
    json += "}";

    Serial.println(json);

    if (config.firebaseHabilitado)
      enviarFirebase(json);
    else Serial.println("Firebase desabilitado");

    salvarUltimaLeitura(ultimaLeitura);
    adicionaHistoricoJson(json);
    Serial.println("Leitura registrada: " + json);

    verificaAlarme(volume, tempInt, umidInt, ts);
  }
}
