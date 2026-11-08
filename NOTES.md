# MTG Life Counter — notas do projeto

Contexto para continuar este projeto no Claude Code (CLI). Leia este arquivo e os
arquivos da pasta antes de começar.

## Hardware
- **Placa:** Seeed Studio XIAO nRF52840 (BLE). A unidade usada se identifica como
  *XIAO nRF52840 Sense* no bootloader (pinagem usada pelo sketch é a mesma).
  Antes rodava um firmware ZMK ("Wake Button"); backup em `backup/` (fora do git).
- **Tela:** Seeed Studio Round Display for XIAO
  - 1.28" IPS 240x240, driver GC9A01, touch capacitivo CHSC6X
  - Tem RTC, slot microSD, carregador de bateria e chave liga/desliga embutidos
- **Bateria:** LiPo 3.7V, 1 célula, conector **JST 1.25 mm** (2 pinos), 300–500 mAh,
  ligada no conector da Round Display (não nos pads do XIAO).
  - **Atenção:** o rabicho JST comprado veio com as cores invertidas em relação à
    serigrafia +/− da placa. Trocar os pinos no conector (vermelho no +) antes de
    soldar na bateria e medir com multímetro antes de plugar.
- **Carregamento:** pelo USB-C do XIAO (o chip da Round Display carrega a bateria).
- **Liga/desliga:** chave deslizante na borda da Round Display (ON = bateria alimenta
  o XIAO e a tela; OFF = desligado de verdade). Se o XIAO estiver no USB, a chave não
  corta a alimentação dele.
- **DIP switch pequeno** da Round Display: deixar em ON (habilita backlight em D6 e
  leitura da bateria em A0). Na outra posição, libera A0 e D6.

## Pinos usados pela Round Display
- Touch: I2C (endereço 0x2E, TWIM0, SDA=4/SCL=5, 100 kHz), interrupção em D7
- Backlight: D6
- Leitura de bateria: A0 (divisor 1/2)
- SPI da tela (SPIM3): SCLK D8, MOSI D10, CS D1, DC D3

## Software / ambiente
- **arduino-cli** 1.5.1 em `~/.local/bin` (também funciona na Arduino IDE)
- Pacote de placa: **Seeeduino:nrf52** 1.1.13 ("Seeed nRF52 Boards"; NÃO usar a
  versão mbed). FQBN: `Seeeduino:nrf52:xiaonRF52840`
- Bibliotecas: **Seeed_GFX** (fork do TFT_eSPI) e **Seeed_Arduino_FS** (dependência
  da Seeed_GFX), ambas instaladas via git em `~/Arduino/libraries`
- O build precisa de `python` (atalho para python3 em `~/.local/bin`) e do
  `adafruit-nrfutil` (instalado via pipx)
- `driver.h` na pasta do sketch com `#define BOARD_SCREEN_COMBO 501`

### Compilar e gravar
```
arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 mtg_life_counter
arduino-cli upload  -b Seeeduino:nrf52:xiaonRF52840 -p /dev/ttyACM0 mtg_life_counter
```
- O upload pela serial reinicia a placa no bootloader sozinho (não precisa de reset
  manual), mesmo se o sketch estiver travado. O usuário precisa estar no grupo
  `dialout`.
- Alternativa: duplo clique no reset → aparece o drive `XIAO-SENSE` → copiar um
  `.uf2` (gerado com `tools/uf2conv/uf2conv.py -f 0xADA52840` a partir do `.hex`).
- Log de debug: serial a 115200 (`DEBUG 1` no topo do sketch). O sketch espera até
  3 s pelo monitor no boot.

## Arquivos
- `mtg_life_counter/mtg_life_counter.ino` — sketch principal (a Arduino IDE exige
  que a pasta tenha o mesmo nome do .ino)
- `mtg_life_counter/driver.h` — seleção de placa/tela para Seeed_GFX
- `NOTES.md` — este arquivo
- `backup/` — firmware ZMK original do XIAO (ignorado pelo git)

## Como a interface funciona
- **Slider circular na borda:** arrastar o dedo pela borda (> 80 px do centro).
  Sentido horário = +1, anti-horário = -1. Cada 15° = 1 de vida (24 por volta).
  **Bolinha laranja** segue o dedo na borda.
- **Toque à direita do número:** +1. **À esquerda:** -1.
- **Segurar à direita/esquerda:** +10/-10 após 600 ms, repetindo a cada 600 ms.
- **Toque bem no centro** (< 30 px): abre o menu — Reiniciar / Standard 20 /
  Commander 40 / Voltar (o modo atual tem borda verde; tocar fora não fecha).
- **Indicador "+3" / "-2"** acima do número, some 2 s após a última mudança.
- **Cores do número:** branco > 10, amarelo ≤ 10, laranja ≤ 5, vermelho ≤ 0.
- **Bateria em %** na parte de baixo, atualiza a cada 30 s (vermelho abaixo de 20%).
- **Salvamento:** vida e modo são salvos na flash 3 s após a última mudança (com o
  dedo fora da tela) e restaurados no boot.

## Decisões técnicas (e por quê)
- **Vídeo próprio (framebuffer + DMA):** o caminho da Seeed_GFX no nRF52840 manda
  cada byte numa transação SPI separada, lento demais. O sketch desenha tudo num
  `TFT_eSprite` 240x240 (115 KB de RAM) e envia só o retângulo alterado, uma
  transferência EasyDMA por linha, com SPI a 32 MHz (`SPI_32MHZ`; usar false = 16 MHz
  se aparecer chuvisco). Tela inteira ≈ 56 ms; mudança de número ≈ 16 ms.
  Redesenho limitado a ~60 fps (`FRAME_MS`), touch lido a cada loop.
- **Touch:** o pino INT do CHSC6X **pulsa** a cada nova leitura, não fica em LOW
  enquanto o dedo está na tela. O fim do toque é detectado por tempo
  (`RELEASE_MS` = 80 ms sem leitura válida).
- **I2C com timeout:** o `Wire.requestFrom` do core espera o hardware sem timeout e
  travava o sketch quando uma leitura do touch se perdia durante arrastos. O sketch
  lê direto no TWIM0 com timeout de 5 ms e faz recuperação do barramento (9 pulsos de
  SCL + STOP + `Wire.begin()`). Timeouts acontecem de vez em quando e são absorvidos.
- **Filtro de saltos:** leituras com mais de 60° de diferença da anterior são
  ignoradas (`MAX_JUMP_DEG`) — evitavam inversão de sentido no slider.
- **Salvamento direto na flash (NVMC):** log append-only de registros de 12 bytes
  (seq + vida + modo + checksum) em duas páginas: 0xF2000 e 0xF3000 (fim da área
  reservada ao InternalFS, que não é tocada por uploads de firmware). Salvar = 3
  escritas de word (< 1 ms); só apaga a outra página quando a atual enche (341
  registros, ~85 ms). No boot vale o registro válido de maior seq. Substituiu o
  LittleFS, que levava ~640 ms por salvamento.
  **Se o BLE (SoftDevice) for ativado, trocar por `sd_flash_write`/`sd_flash_page_erase`.**
- Constante do ADC nesse core: `AR_INTERNAL_2_4` (não `AR_INTERNAL2V4`).

## Ajustes no topo do sketch
- `STEP_DEGREES` — sensibilidade do slider
- `RIM_INNER_R` — onde começa a área da borda
- `CENTER_R` — raio da zona que abre o menu
- `HOLD_MS` — tempo para o +10/-10 ao segurar
- `INVERT_DIRECTION` — inverter se horário estiver subtraindo
- `SPI_32MHZ`, `FRAME_MS`, `RELEASE_MS`, `MAX_JUMP_DEG`, `SAVE_DELAY_MS`

## Status (2026-11-08)
- Testado no hardware via USB: tela, orientação do touch, sentido do slider,
  fluidez, toques laterais, menu e salvamento/restauração funcionando.
- Ainda não testado: funcionamento na bateria (cabo pendente de correção) e se a
  porcentagem da bateria é plausível (fórmula aproximada: ADC 12 bits, referência
  interna 2.4V, divisor 1/2, faixa 3.3V–4.2V).

## Ideias para próximos passos
- Fonte mais bonita para os números (fonte custom em vez da fonte 8 do TFT_eSPI)
- Temas por cor de mana (W/U/B/R/G)
- Contador de veneno / dano de comandante
- Desligar a tela / sleep após inatividade para economizar bateria
- Case impresso em 3D com acesso à chave liga/desliga e ao USB-C
- Usar o BLE do nRF52840 para sincronizar vários contadores (ver nota da flash)
- Desligar `DEBUG` na versão final
