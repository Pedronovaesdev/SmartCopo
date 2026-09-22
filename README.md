# SmartCopo

Monitor inteligente de bebidas para restaurantes, simulado no [Wokwi](https://wokwi.com) com um ESP32.

O dispositivo mede a temperatura e o nível da bebida em um copo e avisa o garçom (LED + buzzer + MQTT) quando a bebida esquenta ou está acabando. O garçom confirma o atendimento apertando um botão físico.

Projeto original: https://wokwi.com/projects/475888782649981953

## Como funciona

- A cada 1 segundo o firmware lê a temperatura (sensor NTC) e a distância até o líquido (sensor ultrassônico HC-SR04), converte a distância em nível percentual do copo e atualiza o display OLED.
- Se a temperatura passar de **10 °C** ou o nível cair abaixo de **25%**, o dispositivo entra em alerta: LED correspondente pisca, o buzzer bipa a cada 2s, e uma mensagem é publicada via MQTT.
- Histerese (1 °C / 5%) evita que o alerta fique ligando e desligando no limite.
- O botão "Atendido" silencia o alerta (LED fica aceso fixo) até a bebida ser reposta e os valores normalizarem.
- Se o sensor de distância não enxergar nada por perto (> 25 cm), o sistema assume que não há copo na mesa.
- Status e eventos são publicados no broker público MQTT `broker.hivemq.com`, tópico base `smartcopo/demo/mesa05`.

### Pinagem (ESP32)

| Componente                          | Pino(s)         |
|--------------------------------------|-----------------|
| Sensor NTC 10k (temperatura)         | GPIO 34         |
| HC-SR04 (nível) — TRIG / ECHO        | GPIO 5 / GPIO 18|
| Display OLED SSD1306 (I2C) — SDA/SCL | GPIO 21 / GPIO 22|
| LED verde / amarelo / vermelho       | GPIO 26 / 27 / 32|
| Buzzer                               | GPIO 25         |
| Botão "Atendido"                     | GPIO 4          |

## Estrutura do projeto

```
SmartCopo.ino       # Firmware (Arduino/ESP32)
diagram.json         # Esquemático do circuito para o simulador Wokwi
wokwi.toml            # Configuração do Wokwi (aponta para o binário compilado)
libraries.txt         # Bibliotecas Arduino usadas pelo projeto
```

## Rodando a simulação no VS Code

O simulador do Wokwi não compila o código sozinho — ele só executa um binário já compilado (`.bin`/`.elf`). Por isso é preciso montar o ambiente de compilação Arduino/ESP32 antes de dar play na simulação.

### 1. Pré-requisitos

- [VS Code](https://code.visualstudio.com/)
- Extensão **[Wokwi Simulator](https://marketplace.visualstudio.com/items?itemName=Wokwi.wokwi-vscode)**
- Extensão **Arduino** para VS Code (ex.: [moozzyk.arduino](https://marketplace.visualstudio.com/items?itemName=moozzyk.vscode-arduino)) — opcional, só facilita compilar pela interface
- **[Arduino CLI](https://arduino.github.io/arduino-cli/)** instalado e no PATH (é ele quem realmente compila; pode ser instalado via `winget install ArduinoSA.CLI` no Windows)

### 2. Instalar o core do ESP32 e as bibliotecas

```bash
arduino-cli config init
arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

arduino-cli lib install "Adafruit GFX Library" "Adafruit SSD1306" "PubSubClient"
```

### 3. Compilar o firmware

> O `arduino-cli` exige que o arquivo `.ino` principal tenha **o mesmo nome da pasta do projeto** — por isso o sketch se chama `SmartCopo.ino` (pasta `SmartCopo/`).

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir build .
```

Isso gera `build/SmartCopo.ino.bin` e `build/SmartCopo.ino.elf`, que é para onde o [wokwi.toml](wokwi.toml) aponta:

```toml
[wokwi]
version = 1
firmware = 'build/SmartCopo.ino.bin'
elf = 'build/SmartCopo.ino.elf'
```

### 4. Simular

Com o projeto aberto no VS Code, abra a paleta de comandos (`Ctrl+Shift+P`) e rode **"Wokwi: Start Simulator"**. O simulador lê o `diagram.json` (circuito) e o `wokwi.toml` (firmware compilado) automaticamente.

Sempre que o código-fonte (`SmartCopo.ino`) for alterado, é preciso repetir o passo 3 (recompilar) antes de simular de novo.
