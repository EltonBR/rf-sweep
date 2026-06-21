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
- `src/ui/app.c`: montagem da janela GTK, controles principais e ciclo da aplicacao.
- `src/ui/app.h`: API publica da UI.
- `src/ui/app_internal.h`: estado interno compartilhado entre modulos da UI.
- `src/ui/app_scan.c`: captura SoapySDR, sweep, FFT e acumulacao de bins.
- `src/ui/app_render.c`: renderizacao do grafico, eixos, legenda e cursor.
- `src/ui/app_io.c`: importar, exportar, limpar e gerar leituras derivadas.
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
10. Use `Subtrair` para criar uma nova curva com a primeira leitura menos todas
    as demais leituras exibidas, calculada em dB na faixa comum entre elas.
11. Marque `Ajustar janela` para ver todo o scan sem scroll horizontal.

A captura usa resolucao horizontal configuravel de 500 Hz a 2.5 kHz por pixel.
A exportacao salva device, inicio/fim em Hz, sample rate, ganho, Hz/pixel,
tempo total do sweep em segundos, cor automatica, nome e dados da leitura mais
recente. Ao mover o mouse sobre o grafico, o status mostra a frequencia e a
potencia de cada leitura naquela posicao.

O importador aceita somente arquivos `.rfsweep` no formato atual `RF_SWEEP_V2`.
Leituras salvas por versoes anteriores sao ignoradas.

Quando o espectro e comprimido para caber na tela ou no PNG, cada pixel desenhado
usa o maior nivel encontrado dentro da faixa de frequencia correspondente. Isso
evita que sinais estreitos, como CW, desaparecam entre pontos amostrados do
grafico.

Durante o sweep, o RF Sweep sintoniza cada frequencia, consulta a frequencia real
retornada pelo driver, abre o stream, espera a sintonia, descarta buffers
iniciais do SDR e entao processa varios FFTs por passo. O passo entre sintonias usa 50% do sample rate para manter
sobreposicao forte entre janelas. O DSP remove offset DC das amostras, aplica
janela Hann, executa FFT complexa, remove apenas alguns bins centrais para
suprimir spur de DC/LO e acumula potencia linear por bin sem cortar as bordas do
FFT. O arquivo salva media e pico; o grafico usa o pico por bin, porque em um
sweep amplo uma portadora estreita nao deve ser diluida por leituras sobrepostas
de ruido. O eixo em dB e relativo ao nivel digital recebido pelo SDR, nao uma
medida calibrada em dBm; portanto o numero absoluto pode diferir do SDRangel,
mas o pico deve aparecer claramente acima do piso de ruido quando o ganho e a
antena estao iguais.

O eixo Y se ajusta automaticamente ao menor e maior sinal encontrados. O campo
`Argumentos SoapySDR` e editavel para usar outro dispositivo, serial ou driver,
por exemplo:

```text
driver=rtlsdr,serial=00000001
```

O controle `Averaging` suaviza a visualizacao com media movel sem alterar os
dados brutos capturados. As opcoes sao `Off`, `MOV 3`, `MOV 5`, `MOV 9`,
`MOV 15` e `MOV 31`; `MOV 5` e o padrao.

## Sample rates e limites dos presets

O sample rate e uma lista fechada por preset. Isso evita valores intermediarios
que o driver pode arredondar internamente e que deslocam o eixo de frequencia do
FFT. O ganho continua em slider, limitado por preset.

| Dispositivo | Sample rates permitidos | Ganho |
|---|---:|---:|
| RTL-SDR | 1.024, 1.8, 2.048, 2.4, 2.56, 2.88, 3.2 MS/s | 0 a 50 dB |
| HackRF One | 2, 2.5, 4, 5, 8, 10, 12.5, 16, 20 MS/s | 0 a 76 dB |
| Mirics/SDRplay | 2.048, 2.4, 3.2, 4, 6, 8, 10 MS/s | 0 a 50 dB |
| SoapySDR personalizado | 0.25, 0.5, 1, 1.024, 1.8, 2, 2.048, 2.4, 2.5, 2.56, 2.88, 3.2, 4, 5, 6, 8, 10, 12.5, 16, 20 MS/s | 0 a 80 dB |

Esses sao valores conservadores do app, nao uma lista universal de todos os
modos possiveis de cada driver. Durante o scan, o programa ainda consulta o
sample rate efetivo com `SoapySDRDevice_getSampleRate()` e usa esse valor real
para calcular o passo do sweep e o eixo de frequencia do FFT.

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
