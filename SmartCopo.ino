/*
  SmartCopo - Monitor inteligente de bebidas para restaurantes
  -------------------------------------------------------------
  O ESP32 mede a temperatura e o nivel da bebida no copo e avisa o garcom
  quando a bebida esquenta ou esta acabando.

  Componentes (simulados no Wokwi):
    - Sensor NTC 10k (temperatura)        -> GPIO 34
    - HC-SR04 ultrassonico (nivel)        -> TRIG 5 / ECHO 18
    - Display OLED SSD1306 (I2C)          -> SDA 21 / SCL 22
    - LED verde / amarelo / vermelho      -> GPIO 26 / 27 / 32
    - Buzzer                              -> GPIO 25
    - Botao "Atendido" (garcom confirma)  -> GPIO 4
  Aviso remoto: MQTT no broker publico HiveMQ, topico smartcopo/demo/mesa05/#

  LED piscando + buzzer = chamado aguardando o garcom
  LED aceso fixo        = garcom ja confirmou (apertou o botao)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ======== Configuracoes da mesa e limites ========
const char* MESA            = "05";
const float TEMP_LIMITE     = 10.0;  // C: acima disso a bebida nao esta mais gelada
const float TEMP_HISTERESE  = 1.0;   // evita o alerta ficar ligando/desligando no limite
const int   NIVEL_BAIXO     = 25;    // %: abaixo disso a bebida esta acabando
const int   NIVEL_HISTERESE = 5;
const float DIST_CHEIO      = 3.0;   // cm entre o sensor e o liquido com o copo cheio
const float DIST_VAZIO      = 15.0;  // cm entre o sensor e o fundo do copo
const float DIST_SEM_COPO   = 25.0;  // acima disso considera que nao ha copo

// ======== Pinos ========
const int PIN_NTC      = 34;
const int PIN_TRIG     = 5;
const int PIN_ECHO     = 18;
const int LED_VERDE    = 26;
const int LED_AMARELO  = 27;
const int LED_VERMELHO = 32;
const int PIN_BUZZER   = 25;
const int PIN_BOTAO    = 4;

// ======== Rede ========
const bool  USAR_MQTT   = true;
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_SENHA  = "";
const char* MQTT_BROKER = "broker.hivemq.com";
const char* TOPICO_BASE = "smartcopo/demo/mesa05";

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ======== Estado ========
float temperatura = 0, distancia = 0;
int   nivel = 0;
bool  alertaTemp = false, alertaNivel = false, semCopo = false;
bool  atendido = false, wifiAvisado = false;
unsigned long ultimaLeitura = 0, ultimoBip = 0, ultimaTentativaMqtt = 0;
unsigned long contadorLeituras = 0;

// ---------- Sensores ----------
float lerTemperatura() {
  int adc = constrain(analogRead(PIN_NTC), 1, 4094);
  const float BETA = 3950;
  return 1 / (log(1 / (4095.0 / adc - 1)) / BETA + 1.0 / 298.15) - 273.15;
}

float lerDistancia() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duracao = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duracao == 0) return 999;          // sem eco = nada na frente do sensor
  return duracao * 0.0343 / 2.0;         // cm
}

int calcularNivel(float d) {
  float n = (DIST_VAZIO - d) / (DIST_VAZIO - DIST_CHEIO) * 100.0;
  return constrain((int)round(n), 0, 100);
}

// ---------- Comunicacao ----------
String statusTexto() {
  if (semCopo) return "SEM_COPO";
  if (alertaTemp && alertaNivel) return "QUENTE_E_ACABANDO";
  if (alertaTemp) return "ESQUENTANDO";
  if (alertaNivel) return "ACABANDO";
  return "OK";
}

String jsonStatus(const char* evento) {
  return "{\"mesa\":\"" + String(MESA) + "\",\"temp\":" + String(temperatura, 1) +
         ",\"nivel\":" + String(nivel) + ",\"status\":\"" + statusTexto() +
         "\",\"evento\":\"" + evento + "\"}";
}

void publicar(const char* subtopico, const String& msg) {
  if (!USAR_MQTT || !mqtt.connected()) return;
  String topico = String(TOPICO_BASE) + "/" + subtopico;
  mqtt.publish(topico.c_str(), msg.c_str());
}

void conectarRede() {
  if (!USAR_MQTT) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!wifiAvisado) {
    wifiAvisado = true;
    Serial.print("[WiFi] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  }
  if (mqtt.connected() || millis() - ultimaTentativaMqtt < 5000) return;
  ultimaTentativaMqtt = millis();
  String id = "smartcopo-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  if (mqtt.connect(id.c_str())) {
    Serial.println("[MQTT] Conectado ao broker");
    publicar("status", jsonStatus("online"));
  } else {
    Serial.println("[MQTT] Falha ao conectar, tentando de novo em 5 s");
  }
}

// ---------- Logica de alertas ----------
void chamarGarcom(const char* motivo) {
  atendido = false;
  ultimoBip = 0;  // bipa na hora
  Serial.printf(">>> CHAMADO mesa %s: %s\n", MESA, motivo);
  Serial.println(jsonStatus("alerta"));
  publicar("alerta", jsonStatus("alerta"));
}

void atualizarEstados() {
  bool antTemp = alertaTemp, antNivel = alertaNivel;

  semCopo = distancia > DIST_SEM_COPO;
  if (semCopo) { alertaTemp = false; alertaNivel = false; return; }

  if (!alertaTemp && temperatura > TEMP_LIMITE) alertaTemp = true;
  else if (alertaTemp && temperatura < TEMP_LIMITE - TEMP_HISTERESE) alertaTemp = false;

  if (!alertaNivel && nivel < NIVEL_BAIXO) alertaNivel = true;
  else if (alertaNivel && nivel > NIVEL_BAIXO + NIVEL_HISTERESE) alertaNivel = false;

  if (alertaTemp && !antTemp)   chamarGarcom("bebida esquentou");
  if (alertaNivel && !antNivel) chamarGarcom("bebida acabando");

  if (!alertaTemp && !alertaNivel && (antTemp || antNivel)) {
    atendido = false;
    Serial.println("Mesa normalizada (bebida reposta).");
    publicar("status", jsonStatus("normalizado"));
  }
}

void lerBotao() {
  static bool ultimoEstado = HIGH;
  static unsigned long tempoBotao = 0;
  bool leitura = digitalRead(PIN_BOTAO);
  if (leitura == LOW && ultimoEstado == HIGH && millis() - tempoBotao > 200) {
    tempoBotao = millis();
    if ((alertaTemp || alertaNivel) && !atendido) {
      atendido = true;
      noTone(PIN_BUZZER);
      Serial.println("Garcom confirmou o chamado.");
      publicar("atendido", jsonStatus("atendido"));
    }
  }
  ultimoEstado = leitura;
}

void atualizarSaidas() {
  bool pisca = (millis() / 400) % 2;
  digitalWrite(LED_VERDE,    !semCopo && !alertaTemp && !alertaNivel);
  digitalWrite(LED_AMARELO,  alertaTemp  && (atendido || pisca));
  digitalWrite(LED_VERMELHO, alertaNivel && (atendido || pisca));

  if ((alertaTemp || alertaNivel) && !atendido && millis() - ultimoBip > 2000) {
    tone(PIN_BUZZER, 2000, 200);
    ultimoBip = millis();
  }
}

// ---------- Display ----------
void desenharTela() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SmartCopo   Mesa ");
  display.print(MESA);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  if (semCopo) {
    display.setTextSize(2);
    display.setCursor(10, 28);
    display.print("SEM COPO");
    display.display();
    return;
  }

  display.setTextSize(2);
  display.setCursor(0, 15);
  display.print(temperatura, 1);
  display.print((char)248);  // simbolo de grau
  display.print("C");

  display.drawRect(0, 36, 100, 10, SSD1306_WHITE);
  display.fillRect(0, 36, nivel, 10, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(104, 37);
  display.print(nivel);
  display.print("%");

  display.setCursor(0, 54);
  if (alertaTemp && alertaNivel) display.print("QUENTE E ACABANDO");
  else if (alertaTemp)           display.print("BEBIDA ESQUENTANDO!");
  else if (alertaNivel)          display.print("BEBIDA ACABANDO!");
  else                           display.print("Bebida gelada - OK");
  if ((alertaTemp || alertaNivel) && atendido) {
    display.fillRect(0, 53, 128, 11, SSD1306_BLACK);
    display.setCursor(0, 54);
    display.print("Garcom a caminho...");
  }
  display.display();
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARELO, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BOTAO, INPUT_PULLUP);

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED nao encontrado");
  display.cp437(true);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("SmartCopo iniciando...");
  display.display();

  if (USAR_MQTT) {
    WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);
    mqtt.setServer(MQTT_BROKER, 1883);
  }
  Serial.println("SmartCopo pronto. Limites: temp > 10 C ou nivel < 25% chamam o garcom.");
}

void loop() {
  conectarRede();
  if (USAR_MQTT) mqtt.loop();
  lerBotao();

  if (millis() - ultimaLeitura >= 1000) {
    ultimaLeitura = millis();
    temperatura = lerTemperatura();
    distancia   = lerDistancia();
    nivel       = calcularNivel(distancia);
    atualizarEstados();
    desenharTela();
    Serial.printf("Mesa %s | %.1f C | nivel %d%% (%.1f cm) | %s\n",
                  MESA, temperatura, nivel, distancia, statusTexto().c_str());
    if (++contadorLeituras % 5 == 0) publicar("status", jsonStatus("leitura"));
  }

  atualizarSaidas();
}
