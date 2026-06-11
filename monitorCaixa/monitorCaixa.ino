

#include <dummy.h>

/*
  Firmware final - Caixa d'água 
 
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

// ------------------ CONFIG ------------------
//const char* WIFI_SSID = "Sitio M&M";
//const char* WIFI_PASS = "ranchofeliz";

const char* WIFI_SSID = "ZTE_DA98";
const char* WIFI_PASS = "2tDusEH5Au";

const char* TELEGRAM_BOT_TOKEN = "8324004011:AAFIn_tryjr7QMVRXlbgwqTikvvHF--VhuE";
const char* TELEGRAM_CHAT_ID = "8283613876";

const int MAX_HISTORICO = 300;
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
    long duracao = pulseIn(ECHO_PIN, HIGH, 60000);

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
const float ALTURA_CAIXA = 150.0;                           // cm
const float RAIO_CAIXA = 112.5;                             // cm
const float AREA_BASE = 3.14159 * RAIO_CAIXA * RAIO_CAIXA;  // cm²


float calcularVolumeLitros(float distancia) {
  float alturaAgua = ALTURA_CAIXA - distancia;
  if (alturaAgua < 0) alturaAgua = 0;
  if (alturaAgua > ALTURA_CAIXA) alturaAgua = ALTURA_CAIXA;

  float volumeCm3 = AREA_BASE * alturaAgua;
  float volumeLitros = volumeCm3 / 1000.0;

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
  String request = String("GET /bot") + TELEGRAM_BOT_TOKEN + "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID + "&text=" + msgEncoded + " HTTP/1.1\r\n";
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
  body { font-family: Arial; margin:20px; background:#f0f2f5; }
  h1   { text-align:center; }
  .card {
      background:white;
      padding:15px;
      margin-bottom:20px;
      border-radius:10px;
      box-shadow:0 2px 6px rgba(0,0,0,0.15);
  }
  canvas { width:100% !important; max-width:700px; }
</style>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>

<body>
<h1>Capituva Monitor</h1>

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
  const dist  = dados.map(x => x.volume);
  const temp  = dados.map(x => x.tempInterna);
  const umid  = dados.map(x => x.umidadeInterna);

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
unsigned long epochNTP  = 0;   // segundos

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
const char* API_KEY = "AIzaSyAlRIiLVigM39ELJ8bXJUIrcIzrviCAHFg";
const char* DATABASE_URL = "https://automacaositiommcapituva-default-rtdb.firebaseio.com";

WiFiClientSecure firebaseClient;


void enviarFirebase(const String& json) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase: sem WiFi");
    return;
  }

  firebaseClient.setInsecure();  // ignora certificado SSL

  String path = String("/capituva_monitor/historico.json?auth=") + API_KEY;

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

bool carregarUltimaLeitura(unsigned long &valorOut) {
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

unsigned long ultimaLeitura = -300;
const unsigned long intervaloLeitura = 30UL;  // 5 minutos em ms


// ------------------ setup() ------------------
void setup() {
  Serial.begin(115200);
  delay(10);
  Serial.println("Inicio do setup.");
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  dht.begin();

  // WiFi
  Serial.printf("Conectando WiFi %s ...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) break;  // timeout 20s
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado: " + WiFi.localIP().toString());
    setupNTP();
  } else {
    Serial.println("\nFalha conectar WiFi. Continuando sem internet.");
    Serial.println(WiFi.status());
  }
  digitalWrite(TRIG_PIN, LOW);
  // HTTP endpoints

   Serial.println("\nIniciando LittleFS...");
  if (!LittleFS.begin()) {
    Serial.println("LittleFS falhou!");
    while (true);
  }
  Serial.println("LittleFS OK.");

  if (!carregarUltimaLeitura(ultimaLeitura)) {
    ultimaLeitura = ultimaLeitura = -300;  // se não existe, usa padrão
  }

  Serial.printf("Valor inicial de ultimaLeitura = %lu\n", ultimaLeitura);

  server.on("/", handleApp);
  server.on("/api/ultima", handleUltima);
  server.on("/api/historico", handleHistorico);
  server.begin();
  Serial.println("HTTP server iniciado na porta 80");

}

// ------------------ loop() ------------------

// coloque a função de retry antes do loop (se ainda não inseriu)
const int DHT_READ_RETRIES = 5;
const int DHT_READ_RETRY_DELAY_MS = 200;

bool lerDHTComRetries(float &outTemp, float &outHum) {
  outTemp = NAN;
  outHum  = NAN;

  for (int i = 0; i < DHT_READ_RETRIES; ++i) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    Serial.printf("DHT tentativa %d: temp=%s  hum=%s\n", i+1,
                  isnan(t) ? "NAN" : String(t).c_str(),
                  isnan(h) ? "NAN" : String(h).c_str());

    if (!isnan(t) && !isnan(h)) {
      outTemp = t;
      outHum  = h;
      return true;
    }
    delay(DHT_READ_RETRY_DELAY_MS);
  }
  return false;
}

void loop() {
  server.handleClient();
  unsigned long agora = getStableEpoch();
delay(200);
//Serial.printf("Valor atual de ultimaLeitura = %lu\n", ultimaLeitura);
//Serial.printf("Valor atual de agora = %lu\n", agora);
//Serial.printf("Valor atual de diferenca = %lu\n", (agora - ultimaLeitura));
//Serial.printf("Valor atual de intervaloLeitura = %lu\n", intervaloLeitura);

  if ((long)(agora - ultimaLeitura) >= (long)intervaloLeitura) {
    ultimaLeitura = agora;

    // -------- LEITURA DOS SENSORES --------
    long volume = calcularVolumeLitros(lerDistancia());  // cm->l (-1 se timeout) nao tratado

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
    enviarFirebase(json);
    salvarUltimaLeitura(ultimaLeitura);
    adicionaHistoricoJson(json);
    Serial.println("Leitura registrada: " + json);

    // Alerta Telegram
    if (volume < 1000) {
      String alerta = "⚠ ALERTA: Nível baixo!\n";
      alerta += "Volume: " + String(volume) + " l";
      if (!isnan(tempInt)) alerta += "Temp Int: " + String(tempInt) + " C\n";
      if (!isnan(umidInt)) alerta += "Umid Int: " + String(umidInt) + " %\n";
      alerta += "TS: " + ts;
      enviarTelegram(alerta);
      Serial.println("Alerta Telegram enviado.");
    }
  }
}
