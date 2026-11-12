# Medidor de distância UWB

Mede a distância entre duas placas **Makerfabs ESP32 UWB DW3000** (ESP32-WROOM-32E +
Qorvo DWM3000) por tempo de voo de rádio UWB, e mostra o resultado num app web.

**App web:** https://engalfatronic.github.io/medidor-distancia-uwb/

## Como funciona

As placas trocam três mensagens por medição (DS-TWR, *double-sided two-way ranging*),
40 vezes por segundo:

```
initiator (A)  --poll-->   responder (B)
initiator (A)  <--resp--   responder (B)
initiator (A)  --final-->  responder (B)
```

Cada placa calcula a distância com o próprio relógio, e a placa B calcula também o valor
combinado, que não depende da diferença de clock entre os cristais. O app mostra os três
valores (placa A, placa B e combinado) e a diferença entre A e B.

O valor principal passa por uma média aparada de 80 leituras (2 s). Se a placa realmente
se mover, o filtro recomeça para acompanhar.

## O que há no repositório

| Pasta | Conteúdo |
|---|---|
| [src/](src/) | Firmware: `initiator.cpp` e `responder.cpp` |
| [include/](include/) | Configuração de rádio e calibração (`uwb_config.h`), filtros, servidor web |
| [data/](data/) | App web (HTML/CSS/JS, sem build). Servido pelas placas, pelo GitHub Pages e embutido no app Android |
| [android-app/](android-app/) | App Android (Capacitor) com serial USB por cabo OTG |
| `vendor/` | Biblioteca `Dw3000` da Makerfabs (submódulo git) |

## Gravar as placas

Requer [PlatformIO](https://platformio.org/).

```bash
git clone --recurse-submodules https://github.com/EngAlfatronic/medidor-distancia-uwb.git

pio run -e initiator -t upload   --upload-port COMx   # firmware da placa A
pio run -e initiator -t uploadfs --upload-port COMx   # app web da placa A
pio run -e responder -t upload   --upload-port COMy   # firmware da placa B
pio run -e responder -t uploadfs --upload-port COMy   # app web da placa B
```

As duas placas precisam do firmware compilado a partir do mesmo `include/uwb_config.h`.

## Ver a medição

Há três formas, todas com a mesma interface:

- **Wi-Fi da placa.** Cada placa cria uma rede (`Medidor-UWB` no initiator,
  `Medidor-UWB-2` no responder, senha `medidor123`). Conecte e abra `http://192.168.4.1`.
  A rede desliga sozinha se ninguém conectar em 1 minuto após ligar a placa; reinicie a
  placa para religar.
- **Serial USB no navegador.** Abra o [app hospedado](https://engalfatronic.github.io/medidor-distancia-uwb/)
  no Chrome ou Edge do PC, ligue a placa no USB e clique em **Serial USB**. O app pode ser
  instalado e funciona offline.
- **App Android.** Ligue a placa no celular por cabo OTG. Veja abaixo.

As placas enviam o estado em JSON pela serial (115200 baud) dez vezes por segundo:

```json
{"role":"initiator","valid":true,"dist":1.441,"avg":1.442,"a":1.445,"b":1.446,"age":2,"count":1152,"fails":1,"wifi":true}
```

## App Android

Requer Node.js, JDK 17+ e Android SDK.

```bash
cd android-app
npm install
npm run android:build      # gera android/app/build/outputs/apk/debug/app-debug.apk
npm run android:install    # instala via adb
```

A porta serial pertence a um serviço, então a conexão continua com o app em segundo
plano. Drivers suportados: CP210x, CH340, FTDI, PL2303 e CDC/ACM.

## Calibração e precisão

O atraso de antena fica em `ANT_DLY_INITIATOR`, em `include/uwb_config.h`. Foi calibrado
em um único ponto (1,00 m, visada livre): +1 unidade reduz a leitura em cerca de 4,7 mm.

Medido a 1 m com visada livre: leitura bruta com desvio padrão de cerca de 2 cm, valor
filtrado variando cerca de 2 cm. Outras distâncias ainda não foram validadas.

Dois cuidados:

- **Reflexos.** Se o sinal direto entre as placas estiver fraco e um reflexo dominar, a
  leitura passa a variar dezenas de centímetros. Mantenha as placas em pé, antenas para
  cima, afastadas de metal e da mesa.
- **Distâncias curtas.** Abaixo de cerca de 35 cm o sinal satura o receptor e a leitura
  deixa de ser confiável.

Compilar com `-DUWB_DIAG` troca a saída serial por uma linha por medição com o
diagnóstico do receptor, útil para investigar reflexos.

## Créditos

A biblioteca `Dw3000` foi desenvolvida pela NConcepts e é mantida no repositório da
[Makerfabs](https://github.com/Makerfabs/Makerfabs-ESP32-UWB-DW3000).
