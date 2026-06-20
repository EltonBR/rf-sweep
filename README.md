# RF Sweep

Aplicativo GTK em C para varrer espectro RF usando SoapySDR e gerar uma imagem
do sinal em linha, com frequencia no eixo X e potencia em dB no eixo Y.

O programa usa a API C do SoapySDR diretamente. Nao depende de `rtl_power`,
`hackrf_sweep` ou outros comandos externos em tempo de captura.

- RTL-SDR: `driver=rtlsdr`
- HackRF One: `driver=hackrf`
- Mirics/SDRplay: `driver=miri`

## Build

```sh
./install-deps.sh
make
./rf-sweep
```

## Estrutura

- `src/main.c`: ponto de entrada.
- `src/ui/app.c`: interface GTK, callbacks, renderizacao e controle da aplicacao.
- `src/ui/app.h`: API publica da UI.
- `src/core/rf_model.c`: presets, tipos de leitura, cores, media movel e helpers de dados.
- `src/core/rf_model.h`: tipos e constantes compartilhadas.
- `src/core/rf_dsp.c`: FFT e rotinas matematicas puras.
- `src/core/rf_dsp.h`: API de DSP.
- `src/core/rf_persistence.c`: serializacao de leituras `.rfsweep`.
- `src/core/rf_persistence.h`: API de persistencia.

Para listar dispositivos e modulos SoapySDR:

```sh
SoapySDRUtil --info
SoapySDRUtil --find
```

## Uso

1. Escolha o dispositivo.
2. Ajuste faixa inicial/final, taxa de amostragem, ganho, `Hz/pixel` e `Averaging`.
3. Clique em `Scan`.
4. Acompanhe a barra de progresso.
5. Ao terminar, a captura vira uma leitura nomeada com cor automatica de alto contraste.
6. Use `Importar` para carregar leituras anteriores e comparar no mesmo grafico.
7. Use `Limpar` para remover as leituras da visualizacao.
8. Use `Exportar` para salvar somente a leitura do ultimo scan em um arquivo `.rfsweep`.
9. Use `Exportar PNG` para salvar a visualizacao atual em `23040 x 720` px.

A captura usa resolucao horizontal configuravel de 500 Hz a 2.5 kHz por pixel.
A exportacao salva device, inicio/fim em Hz, sample rate, ganho, Hz/pixel,
tempo total do sweep em segundos, cor automatica, nome e dados da leitura mais
recente. Ao mover o mouse sobre o grafico, o status mostra a frequencia e a
potencia de cada leitura naquela posicao.

O eixo Y se ajusta automaticamente ao menor e maior sinal encontrados. O campo
`Argumentos SoapySDR` e editavel para usar outro dispositivo, serial ou driver,
por exemplo:

```text
driver=rtlsdr,serial=00000001
```

O controle `Averaging` suaviza a visualizacao com media movel sem alterar os
dados brutos capturados. As opcoes sao `Off`, `MOV 3`, `MOV 5`, `MOV 9`,
`MOV 15` e `MOV 31`; `MOV 5` e o padrao.

## Limites dos presets

Os sliders de sample rate e ganho sao ajustados conforme o dispositivo:

| Dispositivo | Sample rate | Ganho |
|---|---:|---:|
| RTL-SDR | 0.25 a 3.2 MS/s | 0 a 50 dB |
| HackRF One | 2 a 20 MS/s | 0 a 76 dB |
| Mirics/SDRplay | 0.25 a 10 MS/s | 0 a 50 dB |
| SoapySDR personalizado | 0.25 a 20 MS/s | 0 a 80 dB |

Esses sao limites conservadores do app. O driver SoapySDR ou o hardware podem
aceitar apenas subconjuntos desses valores.

## Mirics/MiriSDR e antenas

Dispositivos Mirics/MiriSDR podem expor varias entradas de antena ou caminhos
internos de RF, dependendo do modelo, frontend e driver instalado. Durante um
sweep amplo, o driver pode trocar antena, filtro ou caminho de RF conforme a
frequencia sintonizada.

O RF Sweep atualmente nao seleciona antena manualmente. O preset usa:

```text
driver=miri
```

e o programa apenas chama `SoapySDRDevice_setFrequency()` para cada etapa do
sweep. Portanto, a escolha de antena/caminho fica sob responsabilidade do driver
SoapySDR/Miri. Se o driver alternar antenas automaticamente entre faixas, essa
alternancia aparece embutida na leitura.

O arquivo `.rfsweep` salva device args, inicio/fim, sample rate, ganho,
Hz/pixel, cor, nome e dados de potencia, mas nao registra qual antena foi usada
em cada ponto de frequencia.
