/*
  SmartCopo - Monitor inteligente de bebidas para restaurantes
  -------------------------------------------------------------
  O ESP32 mede a temperatura e o nivel da bebida no copo e avisa o garcom
  quando a bebida esquenta ou esta acabando.

  Componentes (simulados no Wokwi):
    - Sensor DS18B20 1-Wire (temperatura)  -> DQ 15 (pull-up 4.7k p/ 3V3)
    - Celula de carga + HX711 (peso/nivel) -> DT 5 / SCK 18
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
#include <HX711.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// Descomente a linha abaixo para imprimir o valor BRUTO do HX711 no Serial
// Monitor e descobrir o CALIBRACAO_HX711 correto (veja README.md).
// #define CALIBRAR_HX711

// Com esta linha ativa, o peso NAO vem da celula de carga real - o firmware
// gera sozinho um ciclo de "copo cheio sendo bebido aos poucos" para
// demonstracao/teste no simulador, sem precisar mexer em nenhum slider.
// Comente esta linha para voltar a ler o HX711 de verdade.
#define SIMULAR_CONSUMO

// ======== Configuracoes da mesa e limites ========
const char* MESA            = "05";
const float TEMP_LIMITE     = 10.0;  // C: acima disso a bebida nao esta mais gelada
const float TEMP_HISTERESE  = 1.0;   // evita o alerta ficar ligando/desligando no limite
const int   NIVEL_BAIXO     = 25;    // %: abaixo disso a bebida esta acabando
const int   NIVEL_HISTERESE = 5;
const float CALIBRACAO_HX711 = -420.0; // fator de calibracao da celula de carga (g por unidade bruta)
const float PESO_COPO_VAZIO = 20.0;   // g: peso estimado do copo vazio (usado so como piso do calculo)
const float PESO_COPO_CHEIO_PADRAO = 220.0; // g: usado so se o copo for colocado com a balanca ainda calibrando
const float PESO_SEM_COPO   = 10.0;   // g: abaixo disso considera que nao ha copo na balanca

// ======== Pinos ========
const int PIN_DS18B20   = 15;
const int PIN_HX_DT     = 5;
const int PIN_HX_SCK    = 18;
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
HX711 balanca;
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensorTemp(&oneWire);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ======== Estado ========
float temperatura = 0, peso = 0;
float pesoCheioAtual = PESO_COPO_CHEIO_PADRAO;  // peso capturado quando o copo foi colocado (cheio)
int   nivel = 0;
bool  alertaTemp = false, alertaNivel = false, semCopo = false;
bool  atendido = false, wifiAvisado = false;
unsigned long ultimaLeitura = 0, ultimoBip = 0, ultimaTentativaMqtt = 0;
unsigned long contadorLeituras = 0;

// ---------- Sensores ----------
float lerTemperatura() {
  sensorTemp.requestTemperatures();
  float t = sensorTemp.getTempCByIndex(0);
  return t == DEVICE_DISCONNECTED_C ? temperatura : t;  // mantem ultima leitura se o sensor nao respondeu
}

#ifdef SIMULAR_CONSUMO
// Gera um ciclo repetido: sem copo -> copo cheio colocado -> esvaziando aos
// poucos (como se alguem estivesse bebendo) -> copo vazio parado -> repete.
float lerPesoSimulado() {
  const unsigned long SEM_COPO_MS = 4000;    // 4s sem copo na mesa
  const unsigned long BEBENDO_MS  = 40000;   // 40s pra "beber" o copo todo
  const unsigned long VAZIO_MS    = 6000;    // 6s com o copo vazio na mesa
  const float PESO_CHEIO_SIM = 230.0;        // g: copo + bebida ao ser colocado

  unsigned long duracaoCiclo = SEM_COPO_MS + BEBENDO_MS + VAZIO_MS;
  unsigned long t = millis() % duracaoCiclo;

  if (t < SEM_COPO_MS) return 0.0;

  t -= SEM_COPO_MS;
  if (t < BEBENDO_MS) {
    float progresso = (float)t / BEBENDO_MS;  // 0.0 (cheio) -> 1.0 (vazio)
    return PESO_CHEIO_SIM - progresso * (PESO_CHEIO_SIM - PESO_COPO_VAZIO);
  }

  return PESO_COPO_VAZIO;  // copo vazio ainda na mesa, aguardando reposicao
}
#endif

float lerPeso() {
#ifdef SIMULAR_CONSUMO
  return lerPesoSimulado();
#endif
  if (!balanca.is_ready()) return peso;  // mantem ultima leitura se a balanca nao respondeu
#ifdef CALIBRAR_HX711
  long bruto = balanca.get_value(5);
  Serial.printf("[CALIBRACAO] valor bruto = %ld  (aplique um peso conhecido e calcule bruto / peso_em_g)\n", bruto);
#endif
  float g = balanca.get_units(5);
  return g < 0 ? 0 : g;
}

int calcularNivel(float g, float cheio) {
  float n = (g - PESO_COPO_VAZIO) / (cheio - PESO_COPO_VAZIO) * 100.0;
  return constrain((int)round(n), 0, 100);
}

// Detecta quando um copo novo foi colocado na balanca e registra o peso dele
// (presumido cheio) como referencia de 100%. A contagem de volume passa a
// ser feita pela variacao de peso a partir desse momento, nao por um valor
// fixo - assim funciona com copos/bebidas de pesos diferentes.
void atualizarCopo() {
  bool semCopoAntes = semCopo;
  semCopo = peso < PESO_SEM_COPO;

  if (!semCopo && semCopoAntes) {
    pesoCheioAtual = max(peso, PESO_COPO_VAZIO + 20.0f);  // evita divisao por valor quase zero
    Serial.printf("Copo novo detectado: %.1f g registrado como 100%%\n", pesoCheioAtual);
  } else if (semCopo && !semCopoAntes) {
    pesoCheioAtual = PESO_COPO_CHEIO_PADRAO;  // volta ao padrao ate o proximo copo ser colocado
  }
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
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_AMARELO, OUTPUT);
  pinMode(LED_VERMELHO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BOTAO, INPUT_PULLUP);

  balanca.begin(PIN_HX_DT, PIN_HX_SCK);
  balanca.set_scale(CALIBRACAO_HX711);
  balanca.tare();  // zera a balanca (deve iniciar vazia, sem copo em cima)

  sensorTemp.begin();
  sensorTemp.setResolution(9);  // conversao mais rapida (~94 ms em vez de 750 ms)

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
  Serial.println("Balanca (HX711) tarada - calibre CALIBRACAO_HX711 se o peso nao bater.");
}

void loop() {
  conectarRede();
  if (USAR_MQTT) mqtt.loop();
  lerBotao();

  if (millis() - ultimaLeitura >= 1000) {
    ultimaLeitura = millis();
    temperatura = lerTemperatura();
    peso        = lerPeso();
    atualizarCopo();
    nivel       = calcularNivel(peso, pesoCheioAtual);
    atualizarEstados();
    desenharTela();
    Serial.printf("Mesa %s | %.1f C | nivel %d%% (%.1f g de %.1f g) | %s\n",
                  MESA, temperatura, nivel, peso, pesoCheioAtual, statusTexto().c_str());
    if (++contadorLeituras % 5 == 0) publicar("status", jsonStatus("leitura"));
  }

  atualizarSaidas();
}
