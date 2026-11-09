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
- Preferir `tools/flash.sh` (compila, grava e espera o sketch voltar sem abrir a porta
  serial nesse meio-tempo; `--log ARQUIVO [s]` grava o log serial depois;
  `--no-compile` só grava). Ele detecta a placa travada no bootloader e avisa.
- O upload pela serial reinicia a placa no bootloader sozinho (não precisa de reset
  manual), mesmo se o sketch estiver travado. O usuário precisa estar no grupo
  `dialout`.
- **Travamento na gravação (só na gravação):** em ~1 de cada 8 uploads (3 em ~25 em
  2026-11-08, todos depois da bateria ligada) o upload termina com "Device programmed",
  mas a placa fica no bootloader (USB 2886:0045) e não responde nem a pedidos de
  controle USB (`EPROTO` ao mexer no DTR). O laço do bootloader atende o USB o tempo
  todo, então a CPU está travada em algum ponto depois do DFU. Não é a porta serial
  sendo aberta logo após o upload (travou sem isso). Causa exata não encontrada; o
  bootloader não roda no uso normal (só em upload ou duplo reset), então o uso não é
  afetado. **Destravar: um clique no botão reset do XIAO.** Com a chave em ON, tirar o
  USB não reinicia (a bateria mantém a placa ligada). Teste não feito: repetir uploads
  com a chave em OFF para ver se a bateria influi. Bootloader instalado: **UF2 Bootloader 0.6.1**
  (tinyusb 0.10.1; o sketch imprime a versão no boot com `DEBUG`). Atualizar o
  bootloader é uma possível correção, mas tem risco de deixar a placa sem boot.
- Alternativa: duplo clique no reset → aparece o drive `XIAO-SENSE` → copiar um
  `.uf2` (gerado com `tools/uf2conv/uf2conv.py -f 0xADA52840` a partir do `.hex`).
- Log de debug: serial a 115200 (`DEBUG 1` no topo do sketch). O sketch espera até
  3 s pelo monitor no boot (só no USB).
- Comandos de teste pela serial (com `DEBUG`), um por linha: `font N`, `life N`,
  `menu`, `dice N`, `home`, `bat N` (finge N% de bateria; -1 = real), `ctup N` (tema binário). Permitem testar telas sem tocar no painel.
- Durante um upload a tela congela ~35 s (a placa está no bootloader): não é travamento.
  O `flash.sh` fecha o log serial antes de compilar, então o log para ~1 min antes.

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
- **Toque bem no centro** (< 30 px): abre o menu, com ícones em vez de texto:
  ↻ reiniciar (borda laranja) / 20 | 40 (o modo atual tem borda verde) / dado | sol
  (brilho) / paleta de 5 cores (tema) | seta para a esquerda (voltar). Tocar fora não
  fecha. Ícones desenhados em `drawIcon()` (enum `Icon`); um `Btn` tem texto ou ícone.
- **Tema:** Menu → paleta → padrão (-) ou W/U/B/R/G. Muda a cor do anel, da bolinha e das
  bordas dos botões (`THEMES[]`).
- **Brilho:** Menu → sol (título com ícone de sol). Funciona como a vida: toque em "-" / "+" = 1%, segurar =
  10% repetindo, arrastar na borda = 1% a cada 5° (`BRIGHT_STEP_DEG`). Faixa 1–100%.
  Muda na hora; ✓ salva. PWM no D6 (`analogWrite`).
- **Apagar tela:** na mesma tela, o botão abaixo da lua alterna nunca (∞) /
  5 s / 10 s / 15 s / 30 s (padrão) / 1 min / 2 min / 5 min (`SLEEP_SECS`). Na flash
  vai o tempo em unidades de 5 s (não o índice), então a lista pode mudar.
  Fora da tela de vida (menu, dado, brilho) o tempo é no mínimo 15 s (`MENU_MIN_SLEEP_S`). Sem toque por esse
  tempo: backlight em 0 + GC9A01 em sleep (0x28, 0x10). O primeiro toque só acorda a
  tela (0x11, 120 ms, 0x29, repinta tudo) e é ignorado, para não mudar a vida sem querer.
- **Modo bateria baixa:** na bateria com ≤ 20% (`LOW_BAT_ON`), o brilho vai para 1%
  (`LOW_BAT_BRIGHT`) e a tela apaga após 5 s (`LOW_BAT_SLEEP_S`; nos menus continua o
  mínimo de 15 s). Não mexe nos valores salvos. Sai com ≥ 25% (`LOW_BAT_OFF`, folga para
  não ficar alternando) ou com o USB ligado. A % fica vermelha enquanto o modo está ativo.
  Na tela de brilho o backlight mostra o valor escolhido (para dar para ajustar).
  Teste pela serial: `bat N` finge N% e ignora o USB; `bat -1` volta à leitura real.
- **Dado:** Menu → dado → escolher d4, d6, d8, d10, d12 ou d20 (ou a seta de voltar). Faz uma
  animação curta e mostra o resultado na fonte grande da vida (verde no máximo,
  vermelho no 1). Fica na tela até um toque, que volta para a vida (a seta embaixo indica isso). O
  `random()` é semeado no boot pelo RNG de hardware (`NRF_RNG`).
- Não há mais rótulo "Standard"/"Commander" embaixo do número.
- **Indicador "+3" / "-2"** acima do número, some 2 s após a última mudança.
- **Cores do número:** branco > 10, amarelo ≤ 10, laranja ≤ 5, vermelho ≤ 0.
- **Bateria em %** na parte de baixo, atualiza a cada 30 s (vermelho abaixo de 20%).
  **Raio amarelo** ao lado = USB conectado (VBUS). O carregador da Round Display não
  informa o estado de carga ao XIAO, então "USB ligado" é o melhor sinal de "carregando".
  A % só funciona com a **DIP "ON KE" nas duas posições em ON** (em OFF o A0 lê ruído ~0,4 V → 0%).
- **Boot:** a tela de vida é desenhada logo após `tft.init()` e o backlight só acende
  com ela pronta (sem splash: o boot já é rápido). A espera de até 3 s pelo monitor
  serial vem depois e só acontece no USB (VBUS); na bateria não há espera.
- **Números grandes / fontes:** 12 fontes anti-aliased (4 bits por pixel), escolhidas em
  Menu → "Aa": 0 Noto Sans Bold (padrão), 1 Cinzel, 2 Bebas Neue, 3 Orbitron, 4 Bungee,
  5 Black Ops One, 6 Press Start 2P, 7 Permanent Marker, 8 Righteous, 9 Bangers,
  10 Creepster, 11 Griffy (Google Fonts, licenças OFL/Apache). Cada uma tem dois
  tamanhos em `mtg_life_counter/fonts/`: `_xl` (dígitos de até 76 px, cabem 2
  caracteres) e `_l` (cabem 4, de -999 a 999). `tools/build_fonts.sh` baixa as fontes e
  gera tudo com `tools/make_bigfont.py` (que acha o maior tamanho que cabe). A ordem
  no script = `FONTS[]` no sketch = valor salvo. ~30 KB de flash por fonte; o sketch
  usa ~69% da flash.
- **Tela de fontes:** número de teste (começa na vida atual, não altera a vida) na
  fonte candidata; bolinhas mostram a posição; ◀ ▶ trocam na hora, ✓ salva. O número
  de teste muda como a vida: toque à direita/esquerda dele ±1, segurar ±10, borda.
- **Salvamento:** vida, fonte, tema, modo, brilho e tempo de apagar são salvos na flash 3 s após a última mudança (com o
  dedo fora da tela) e restaurados no boot.

## Temas pelo site (BLE)
- **Site:** `docs/index.html` + `docs/themes.js` (editor + Web Bluetooth). Publicado copiando os
  dois arquivos para `~/joaopedropio.github.io/mtg-life/` → https://joaopedropio.github.io/mtg-life/
  (os outros projetos do site seguem o mesmo padrão). Chrome/Edge no Android e computador;
  no iPhone só pelo app **Bluefy** (o Safari não tem Web Bluetooth).
- **BLE sempre ligado** (Bluefruit / SoftDevice S140), nome "MTG Life", +4 dBm, anúncio a 100 ms
  nos primeiros 30 s e depois 1 s. Serviço `4d544700-6c69-6665-2d63-6f756e746572`: CTRL
  (`...4701`, write+notify) e DATA (`...4702`, write without response). Protocolo no
  comentário "BLE: themes from the web page" no sketch. **Protocolo v2:** o INFO traz o MTU
  atual no byte 7, e os dados também podem vir pelo CTRL como `06 + bytes` (CTRL aceita até
  244 bytes).
- **iPhone/Bluefy:** o Bluefy **nunca entrega escritas na característica DATA** (sem resposta
  elas "resolvem" e somem; com resposta rejeitam com um erro sem mensagem), mas o CTRL funciona.
  Por isso o site, no iOS, manda os dados pelo CTRL (`06 + dados`, com resposta, 243 bytes):
  ~18–20 s por tema. Nos outros navegadores continua o DATA sem resposta (~5 s); se a primeira
  página não for confirmada, o site recomeça uma vez no modo lento. O MTU no iPhone também é 247
  (na conexão o firmware sempre vê 23; ele sobe logo depois).
- Diagnóstico no log (`DEBUG`): MTU e intervalo na conexão, bytes/escritas recebidos ao
  desconectar e `rx=bytes/escritas up=estado written=` na linha "alive".
- **Velocidade** (medida no Mac com bleak, 58.176 bytes): iPhone/CTRL 18,0 s → **8,9 s**;
  DATA rápido 5,0 s → **3,6 s**. Como: no BEGIN a placa pede intervalo de **15 ms**
  (`CONN_FAST` = 12; o iOS só aceita min = max = 15 ms) e volta a 30 ms (`CONN_IDLE`) no END;
  PHY 2M pedido na conexão; e cada página do espaço é **apagada só quando os dados dela chegam**
  (antes o BEGIN apagava tudo em ~2,7 s). Se o envio parar antes da 1ª página, o tema antigo
  continua válido. O log do END mostra o tempo do envio e o intervalo aplicado.
- Compressão (RLE ~10× no tema de teste) foi considerada e **o usuário preferiu não fazer**.
- Scripts de teste pelo Mac (no scratchpad, recriar se precisar): cópia do
  `tools/ble_theme_test.py` com `write_gatt_char(CTRL, b"\x06" + parte, response=True)` e
  `chunk = 243` simula o caminho do iPhone. O `ble_theme_test.py` agora só envia quando
  executado direto (importá-lo para usar `make_record()` não envia mais nada).
- **Com o SoftDevice ligado a flash é gravada por `sd_flash_*`** (`flashErasePage` /
  `flashWriteWords` decidem sozinhas). O RNG é lido antes do `bleBegin()`.
- **Espaços de tema:** 2 espaços de 0xF000 em 0xCF000–0xED000. Registro = cabeçalho de 64 bytes
  (magic "MTGT", tamanho, CRC-32, nome, cores RGB565, efeito do número) + paleta de 256 cores
  + 240×240 índices = 58.176 bytes. O fundo já traz anel e decoração; o firmware desenha número,
  "+3", bateria e bolinha por cima e apaga copiando o fundo da flash (`restoreBg`). Temas 0–5 =
  embutidos, 6–7 = espaços. Só a tela de vida usa a imagem; menus continuam pretos com a cor de
  destaque do tema.
- **O sketch precisa terminar antes de 0xCF000** (hoje ~0xC4000, 79% da flash). Se passar, o
  `ctInit()` desliga os temas personalizados. O upload pela serial só apaga as páginas do
  firmware novo, então os temas sobrevivem a gravações (confirmar se mudar o bootloader).
- Teste sem BLE: comando serial `ctup N` + os 58.176 bytes do registro grava no espaço N.
- O Bluetooth do PC (Intel AX210 na B550M) recebe tudo a ~-95 dBm e não segura conexão
  (desconecta com motivo 0x08). Testar BLE pelo celular ou pelo Mac Mini.

## Rodar no Mac (Mac Mini) — ambiente montado e compilando (2026-11-08, 5ª sessão)
- Mac Mini é Apple Silicon: o `arm-none-eabi-gcc` do pacote Seeed é x86_64 → precisa do
  **Rosetta** (`softwareupdate --install-rosetta --agree-to-license`; já instalado).
- O atalho `python` **não pode ser symlink** para `/usr/bin/python3` (é um stub do Xcode que
  falha com "Failed to locate 'python'"). Usar um script: `#!/bin/sh` + `exec /usr/bin/python3 "$@"`.
- Instalado: arduino-cli, pipx, adafruit-nrfutil, Seeeduino:nrf52 1.1.13, Seeed_GFX 2.0.3,
  Seeed_Arduino_FS 2.1.5. Compila igual ao Linux (642.676 bytes, 79%). Porta: `/dev/cu.usbmodem21101`.
- Na primeira vez o macOS pede permissão de Bluetooth para o terminal e a instalação do
  Command Line Tools (pedida pelo `/usr/bin/python3`); antes disso o scan do bleak fica parado.
- **BLE testado pelo Mac (bleak):** conecta, MTU 247, tema de 58.176 bytes no espaço 1 em 5,0 s
  (11,3 KB/s; apagar o espaço leva 2,7 s), CRC ok, selecionado e salvo.
O PC Linux tem Bluetooth ruim (ver "Temas pelo site"); no Mac dá para gravar pelo USB e
testar o BLE (Chrome e Python/bleak usam o Bluetooth do macOS).
```
brew install arduino-cli pipx
pipx install adafruit-nrfutil
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
arduino-cli core update-index
arduino-cli core install Seeeduino:nrf52@1.1.13
# bibliotecas (no Linux estavam em ~/Arduino/libraries; no Mac o padrão é ~/Documents/Arduino/libraries)
git clone https://github.com/Seeed-Studio/Seeed_GFX ~/Documents/Arduino/libraries/Seeed_GFX          # usada: 2.0.3
git clone https://github.com/Seeed-Studio/Seeed_Arduino_FS ~/Documents/Arduino/libraries/Seeed_Arduino_FS   # usada: 2.1.5
```
- No Linux o build pedia um comando `python` (atalho para python3) e o `adafruit-nrfutil` no
  PATH. Se o compile/upload reclamar de `python` no Mac:
  `ln -s "$(which python3)" /usr/local/bin/python` (ou um diretório próprio no PATH).
- Compilar: `arduino-cli compile -b Seeeduino:nrf52:xiaonRF52840 mtg_life_counter`
- Porta: `ls /dev/cu.usbmodem*` (sem grupo `dialout` no Mac). Gravar:
  `arduino-cli upload -b Seeeduino:nrf52:xiaonRF52840 -p /dev/cu.usbmodemXXXX mtg_life_counter`
- **`tools/flash.sh` é só Linux** (usa `lsusb`, `sg dialout`, `/dev/serial/by-id`). No Mac usar os
  comandos acima, ou adaptar o script (`ioreg`/`system_profiler SPUSBDataType` no lugar do `lsusb`).
- Log serial: `stty -f /dev/cu.usbmodemXXXX 115200 raw -echo; cat /dev/cu.usbmodemXXXX > log.txt`
  (em segundo plano). Comandos de teste: `printf 'menu\n' > /dev/cu.usbmodemXXXX`.
  Upload de tema pelo USB: `printf 'ctup 0\n' > PORTA; sleep 2; cat tema.bin > PORTA`.
- Site local: `python3 -m http.server -d docs 8000` → http://localhost:8000 no Chrome (localhost
  conta como seguro, então o Web Bluetooth funciona).
- Teste BLE em Python: `python3 -m venv .venv && .venv/bin/pip install bleak` e
  `.venv/bin/python tools/ble_theme_test.py 0 [tema.bin]` (o macOS pede permissão de Bluetooth
  para o Terminal na primeira vez).

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
- **Salvamento direto na flash (NVMC):** log append-only de registros de 16 bytes
  (seq + vida + modo + brilho + apagar + fonte + 1 byte reservado + tema + checksum
  "MTG2"; 256 registros por página) em duas páginas: 0xF2000 e 0xF3000 (fim da área
  reservada ao InternalFS, que não é tocada por uploads de firmware). Salvar = 3
  escritas de word (< 1 ms); só apaga a outra página quando a atual enche (256
  registros, ~85 ms). No boot vale o registro válido de maior seq. O byte do tema
  (só os 4 bits baixos valem) era "reservado" (0xFF) antes: 0xFF vira o tema padrão,
  então registros antigos continuam valendo. Registros de 12 bytes (versão "MTG1") não são lidos. Substituiu o
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
- Bateria ligada (polaridade corrigida). Com a DIP em ON a leitura deu 4,16 V no USB.
- `arduino-cli upload` NÃO recompila: sempre rodar `compile` antes.
- 2026-11-08 (2ª sessão), gravado e com boot conferido no log: sem rótulo do modo,
  menu 20/40, dado, fonte grande nos menus, raio de USB na bateria, boot rápido,
  brilho 1–100% (toque/segurar/borda) e apagar tela configurável. Testado pelo
  usuário: dado, brilho, apagar/acordar. Ainda não confirmado na tela: opções
  5 s / 10 s e o mínimo de 15 s fora da tela de vida.
- 2026-11-08 (3ª sessão), gravado e testado pelo usuário: 12 fontes escolhíveis (tela
  "Aa"), temas de mana, menu com ícones, comandos de teste pela serial,
  `tools/flash.sh`. O usuário aprovou a tela de fontes como está (sem desenhar + e −;
  os toques laterais mudam o número de teste). Commitado na 4ª sessão.
  Contadores de veneno / comandante foram feitos e retirados a pedido do usuário. Log de diagnóstico da bateria removido. `DEBUG` continua
  ligado enquanto as novidades não forem testadas na tela (só custa a espera de até
  3 s pelo monitor quando está no USB).
- 2026-11-08 (4ª sessão), gravado: modo bateria baixa (testado pela serial), BLE + temas
  personalizados (2 espaços) + site em `docs/` (publicado em joaopedropio.github.io/mtg-life/).
  Testado: tema enviado pelo site → arquivo → `ctup` pela USB → gravado (sd_flash), CRC ok,
  aplicado e salvo. **Falta testar:** conexão BLE de verdade (celular/Mac/Bluefy no iPhone),
  aparência do tema no aparelho (o usuário ainda não confirmou), o tema voltar após religar,
  e tocar na tela de tema (miniatura do espaço). Mockups de layout/temas em
  `tools/mockups/layouts.html` (layouts A–F da tela de vida e 1–6 do menu ainda não escolhidos).

- 2026-11-08 (5ª sessão), no Mac Mini: ambiente montado (ver "Rodar no Mac"). **Testado e
  aprovado pelo usuário:** tema enviado por BLE de verdade (bleak), aparência no aparelho, tema
  volta depois de desligar e religar na bateria, miniaturas e troca na tela de tema.
  **Site testado no Chrome do Mac** (localhost, Web Bluetooth): conectou e enviou o tema
  "Papel (claro)" em 2,8 s, aplicado. Funciona também no Chrome do Android. **iPhone (Bluefy)**
  funcionou depois do protocolo v2 (dados pelo CTRL; ver "Temas pelo site").
  Site: temas em abas (Elaborados / Magic: cores de mana e guildas / Simples); clicar num tema
  rola até o editor ("Editando: nome", botão "↑ Trocar tema").
  Página na raiz do GitHub Pages (`~/joaopedropio.github.io/index.html`, visual do
  joaopedropio.dev branch `main`, fontes Geist em `assets/fonts/`) com um card por página.
  Velocidade de envio melhorada (ver "Temas pelo site"); firmware gravado, **ainda não medido
  no iPhone**. O espaço 0 ficou com o tema de teste (degradê) por engano: o usuário vai reenviar
  o Engrenagens pelo iPhone, o que também mede a velocidade nova lá.
- No Mac o atalho `python` foi criado só na pasta temporária da sessão; recriar
  (`~/.local/bin/python` com o script de "Rodar no Mac") antes de compilar.

## Ideias para próximos passos
- Case impresso em 3D com acesso à chave liga/desliga e ao USB-C
- Desligar `DEBUG` na versão final
- Escolher layout da tela de vida / menu entre os mockups
- Mais espaços de tema (comprimir com RLE) ou microSD
