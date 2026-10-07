# SmartCopo

Monitor inteligente de bebidas para restaurantes, simulado no [Wokwi](https://wokwi.com) com um ESP32.

O dispositivo mede a temperatura e o nível da bebida em um copo e avisa o garçom (LED + buzzer + MQTT) quando a bebida esquenta ou está acabando. O garçom confirma o atendimento apertando um botão físico.

Projeto original: https://wokwi.com/projects/475888782649981953

## Como funciona

- A cada 1 segundo o firmware lê a temperatura (sensor digital **DS18B20**, 1-Wire) e o peso do copo na **célula de carga + HX711**, converte o peso em nível percentual do copo e atualiza o display OLED.
- Se a temperatura passar de **10 °C** ou o nível cair abaixo de **25%**, o dispositivo entra em alerta: LED correspondente pisca, o buzzer bipa a cada 2s, e uma mensagem é publicada via MQTT.
- Histerese (1 °C / 5%) evita que o alerta fique ligando e desligando no limite.
- O botão "Atendido" silencia o alerta (LED fica aceso fixo) até a bebida ser reposta e os valores normalizarem.
- Se o peso na balança ficar abaixo de 10g, o sistema assume que não há copo na mesa.
- Status e eventos são publicados no broker público MQTT `broker.hivemq.com`, tópico base `smartcopo/demo/mesa05`.

### Pinagem (ESP32)

| Componente                            | Pino(s)           |
|----------------------------------------|-------------------|
| DS18B20 (temperatura, 1-Wire)           | GPIO 15 (+ pull-up 4.7kΩ p/ 3V3) |
| Célula de carga + HX711 (peso/nível) — DT / SCK | GPIO 5 / GPIO 18  |
| Display OLED SSD1306 (I2C) — SDA/SCL    | GPIO 21 / GPIO 22 |
| LED verde / amarelo / vermelho         | GPIO 26 / 27 / 32 |
| Buzzer                                 | GPIO 25           |
| Botão "Atendido"                       | GPIO 4            |

> A célula de carga precisa ser calibrada: ajuste `CALIBRACAO_HX711` e `PESO_COPO_VAZIO` no [SmartCopo.ino](SmartCopo.ino) para a sua balança real. No simulador, o peso aplicado ao HX711 é ajustado pelo slider que aparece ao clicar no componente durante a simulação.
>
> O nível não usa um peso "cheio" fixo: quando um copo é colocado na mesa (peso sobe acima de `PESO_SEM_COPO`), o firmware registra esse peso como referência de 100% e calcula o volume consumido a partir da **variação de peso** desse momento em diante — assim funciona com copos e bebidas de pesos diferentes, não só com um valor fixo de calibração.

### Modo de demonstração (peso simulado automaticamente)

Por padrão, o firmware está com `#define SIMULAR_CONSUMO` **ativo** no topo do [SmartCopo.ino](SmartCopo.ino). Com isso, o peso não vem da célula de carga real — o código mesmo gera um ciclo repetido, sem precisar mexer em nenhum slider:

1. **4s** sem copo na mesa (`SEM_COPO`)
2. Um copo "cheio" (230g) aparece e vai sendo "bebido" aos poucos ao longo de **40s**, até sobrar só o peso do copo vazio
3. **6s** com o copo vazio parado na mesa
4. Repete o ciclo

É a forma mais simples de ver o nível caindo, o alerta de "bebida acabando" disparando, o botão "Atendido" silenciando o buzzer, etc., direto no simulador.

Para voltar a usar a célula de carga/HX711 de verdade (e o slider manual de peso), comente essa linha:
```cpp
// #define SIMULAR_CONSUMO
```
e recompile.

### Calibrando a célula de carga (HX711)

> Só é necessário se `SIMULAR_CONSUMO` estiver **desativado** (ou ao montar o circuito físico de verdade).

O fator `CALIBRACAO_HX711` converte o valor bruto do sensor em gramas, e esse fator **não é universal** — depende da célula de carga (real) ou do chip simulado (Wokwi), então sempre precisa ser medido:

1. No [SmartCopo.ino](SmartCopo.ino), descomente a linha `// #define CALIBRAR_HX711` perto do topo do arquivo.
2. Recompile (`arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir build .`) e rode o simulador.
3. Com a balança sem peso nenhum (slider do HX711 em 0), deixe o firmware iniciar e tarar.
4. Aplique um peso conhecido no slider "Pressure" do componente (ex.: exatamente **1.000 kg** = 1000 g).
5. No Serial Monitor vai aparecer algo como `[CALIBRACAO] valor bruto = -420000`.
6. Calcule: `CALIBRACAO_HX711 = valor_bruto / peso_em_gramas` (no exemplo: `-420000 / 1000 = -420`).
7. Coloque esse número na constante `CALIBRACAO_HX711`, comente a linha `#define CALIBRAR_HX711` de novo, e recompile.

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

arduino-cli lib install "Adafruit GFX Library" "Adafruit SSD1306" "PubSubClient" "HX711 Arduino Library" "OneWire" "DallasTemperature"
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
