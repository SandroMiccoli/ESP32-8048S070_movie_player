# Guia de uso — Bocas / Telas

Este documento explica o sistema de forma simples, para quem **não precisa saber programar**.  
A ideia é: ligar, usar e, quando necessário, trocar os vídeos das telas.

Para detalhes técnicos (instalação, comandos, Wi‑Fi, programação das placas), use o [README.md](README.md) e o [rpi_sound_trigger/README.md](rpi_sound_trigger/README.md).

---

## O que é este sistema?

É uma instalação com **várias telas** (as “bocas”) e um **computador pequeno** (Raspberry Pi) com microfone.

Em resumo:

1. As telas ficam mostrando um vídeo de **espera** (calmo, em loop).
2. Quando o microfone detecta um som forte o suficiente, **todas as telas** mudam para um vídeo de **alerta**.
3. Quando o alerta termina, as telas voltam sozinhas para o vídeo de espera.

Não é preciso abrir programas nem digitar comandos no dia a dia. O sistema foi montado para funcionar de forma **turnkey**: liga e pronto.

---

## Como ligar (uso do dia a dia)

O conjunto já vem preparado para subir junto.

1. Ligue a energia do sistema (o botão / a tomada do conjunto, conforme a montagem física).
2. Espere alguns segundos — o Raspberry Pi e as telas ligam e se conectam entre si.
3. As telas devem começar a mostrar o vídeo de **espera**.
4. Quando houver um som acima do limiar configurado, as telas tocam o vídeo de **alerta** e depois voltam ao de espera.

### Se algo não aparecer

| O que a tela mostra | O que isso significa (em linguagem simples) |
| ------------------- | ------------------------------------------- |
| `INSERT SD CARD` | O cartão de memória não está encaixado. |
| `NO /mjpeg FOLDER` | No cartão falta a pasta chamada `mjpeg`. |
| `NO MOVIES TO PLAY` | A pasta existe, mas não tem vídeos no formato certo. |
| `MISSING idle.mjpeg` / `MISSING alert.mjpeg` | Faltam os dois vídeos principais (espera e alerta). |

Nesses casos, desligue, confira o cartão SD daquela tela e ligue de novo.  
Mais detalhes: seção **On-screen messages** no [README.md](README.md).

---

## Como o sistema funciona (visão geral)

Pense em três partes:

```
Microfone (Pi)  →  “tem som alto?”  →  avisa as telas
                                      ↓
                         tela troca: espera → alerta → espera
```

| Parte | Função |
| ----- | ------ |
| **Raspberry Pi** | Escuta o microfone e, quando passa do volume configurado, avisa as telas. |
| **Telas (ESP32)** | Reproduzem os vídeos do cartão SD e reagem ao aviso do Pi. |
| **Cartão SD** | Guarda os arquivos de vídeo (e áudio, se houver) de cada tela. |

As telas e o Pi conversam por uma rede Wi‑Fi própria, criada pelo Pi. Você não precisa de internet para o sistema funcionar.

Documentação técnica dessa parte: [rpi_sound_trigger/README.md](rpi_sound_trigger/README.md) (arquitetura, instalação, sensibilidade do microfone, volumes).

---

## Os dois vídeos de cada tela

Em cada cartão SD existem (pelo menos) **dois** vídeos com nomes fixos:

| Arquivo | Quando aparece |
| ------- | -------------- |
| `idle.mjpeg` | Vídeo de **espera** — fica em loop o tempo todo. |
| `alert.mjpeg` | Vídeo de **alerta** — toca **uma vez** quando o som dispara o sistema. |

Opcionalmente, cada um pode ter áudio com o mesmo nome:

- `idle.mp3`
- `alert.mp3`

Eles ficam dentro da pasta:

```
cartão SD → pasta mjpeg → idle.mjpeg / alert.mjpeg (e os .mp3, se houver)
```

Detalhes da organização do cartão: seção **SD card layout** no [README.md](README.md).

---

## Como os arquivos de vídeo são preparados

Os arquivos que entram no cartão **não são** o arquivo bruto da câmera. Há um caminho em três etapas.

### 1. Gravação original

É o material filmado / gravado na origem (o arquivo “cru”, como veio da câmera ou do projeto).

### 2. TouchDesigner — estética do vídeo

Esse original passa por um projeto no **TouchDesigner**, que aplica a **estética visual** desejada (o “look” das bocas).  
O resultado dessa etapa ainda é um vídeo normal (por exemplo `.mp4`), mas já com a cara final da instalação.

### 3. Conversão com ffmpeg — formato das telas

As placas ESP32 não tocam qualquer vídeo de celular ou YouTube. Elas precisam de um formato específico chamado **MJPEG** (uma sequência de imagens JPEG empacotada num arquivo `.mjpeg`), e o áudio em **MP3** (recomendado).

Essa conversão é feita com o programa **ffmpeg**, no computador.  
Os comandos prontos e as opções de qualidade estão na seção **Video conversion** do [README.md](README.md).

Resumo do fluxo:

```
gravação original
        ↓
  TouchDesigner  (estética)
        ↓
     ffmpeg      (vira .mjpeg + .mp3)
        ↓
  cartão SD da tela  (nomes idle / alert)
```

---

## Como trocar os vídeos

Quando quiser mudar o conteúdo das telas:

1. **Prepare o novo material**  
   Original → TouchDesigner → ffmpeg (como acima), gerando:
   - `idle.mjpeg` (+ `idle.mp3` se quiser som)
   - `alert.mjpeg` (+ `alert.mp3` se quiser som)

2. **Desligue a tela** (ou o sistema) e retire o cartão SD com cuidado.

3. **No computador**, abra o cartão e vá até a pasta `mjpeg`.

4. **Substitua** os arquivos antigos pelos novos, **mantendo exatamente esses nomes**  
   (`idle.mjpeg`, `alert.mjpeg`, etc.).  
   Se mudar o nome, a tela não encontra o arquivo.

5. **Ejecte / remova com segurança** o cartão, coloque de volta na tela e ligue.

6. Confira se o vídeo de espera aparece e, se possível, teste um som para ver o alerta.

### Dicas importantes

- Cada tela tem o **seu** cartão. Se todas devem mostrar o mesmo conteúdo, copie os mesmos arquivos para todos os cartões.
- Não apague a pasta `mjpeg` — só troque o que está dentro.
- Arquivos muito longos ou com qualidade alta demais podem ficar grandes; use os presets do README técnico (há uma opção pensada para vídeos longos de ~8–9 minutos).

Referência completa: **SD card layout** e **Video conversion** em [README.md](README.md).

---

## O que você normalmente *não* precisa fazer

No uso cotidiano, **não** é necessário:

- Abrir o Arduino IDE nem “gravar” o programa nas placas de novo  
- Mudar códigos ou arquivos de configuração  
- Conectar as telas à internet de casa  

Essas etapas já foram feitas na montagem inicial. Só voltam a ser úteis se alguém for **reinstalar**, **trocar uma placa** ou **ajustar Wi‑Fi / sensibilidade**.  
Nesses casos, comece pelo [README.md](README.md) e pelo [rpi_sound_trigger/README.md](rpi_sound_trigger/README.md).

---

## Onde achar mais detalhes

| Assunto | Onde ler |
| ------- | -------- |
| Visão técnica do player nas telas | [README.md](README.md) — início do documento |
| Pasta e nomes no cartão SD | [README.md](README.md) — **SD card layout** |
| Converter vídeo com ffmpeg | [README.md](README.md) — **Video conversion** |
| Mensagens na tela | [README.md](README.md) — **On-screen messages** |
| Microfone, Wi‑Fi do Pi, alerta por som | [rpi_sound_trigger/README.md](rpi_sound_trigger/README.md) |
| Volume de cada boca / sensibilidade | [rpi_sound_trigger/README.md](rpi_sound_trigger/README.md) — interface e `config.yaml` |

---

## Em uma frase

**Liga o sistema → as telas tocam a espera → um som forte dispara o alerta → voltam à espera.**  
Para mudar o conteúdo: prepare o vídeo (TouchDesigner + ffmpeg), copie como `idle` / `alert` na pasta `mjpeg` do cartão SD.
