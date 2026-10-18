# Roadmap

A meta é **120+ fps em 4K com FSR**, em qualquer área do jogo.

## Feito

- Boot, vídeos, menus, cutscenes e gameplay, com áudio, controle e saves.
- Toda a parte SPU traduzida para C:
  - o gerenciador de jobs;
  - 27 módulos de job;
  - os plugins de oclusão;
  - as cadeias de job do Wwise;
  - o Bink e o zlib.
- Oclusão ambiente e iluminação deferida na GPU no lugar dos jobs de SPU.
- Escala de resolução 2× e 3× (até 4K).
- Água, partículas e profundidade de fragmento corretas.
- Dublagem e textos em português (PT-BR).
- Tela cheia com a proporção certa (F11 / Alt+Enter).
- Menu principal (Continuar, Novo jogo, Carregar jogo, Opções, Sair) e opções
  no jogo (F1).
- Desempenho:
  - frame em voo na GPU;
  - menos custo por draw;
  - geometria residente na VRAM;
  - thread de gravação Vulkan;
  - sem esperas da GPU ao descartar texturas.

## Em andamento

| Item | Situação |
|---|---|
| Travamentos raros no meio do jogo | O `play.sh` captura as pilhas das threads quando a imagem para; falta reproduzir |
| Custo por draw na thread do RSX | Em áreas com ~9 mil draws por frame, essa thread, a de gravação, a principal do PPU e os SPUs ficam todos em 84–94% |
| Vazão dos SPUs | Já limita o fps em New Marais; o custo vem do runtime SPU (DMA, despacho) e do código de cada job |
| Primeira compilação de shaders | Pico de um frame na primeira vez de cada efeito; o cache em disco cobre as seguintes |

## Próximos

**Além do PS3** (depois da base de 120 fps estável):

- Filtro anisotrópico 16x forçado e anti-aliasing de verdade (SMAA/FXAA ou
  supersampling pela escala de render).
- FSR para 4K a 120 fps.
- Sombras em resolução mais alta, independente da escala.
- Ultrawide e FOV ajustável (projeção e culling).
- Distância de desenho e LOD maiores, menos pop-in: depende do job `ec5f`
  nativo, que é quem escolhe o que é desenhado e em qual detalhe.
- HDR e pacote de texturas em alta resolução.


1. **Upscaler (FSR):** renderizar abaixo de 4K e escalar com FSR. A rota
   (Vulkan + FSR 3.1 ou D3D12 + FSR 4.1 via Proton) ainda está em aberto.
2. **Vértices sem cópia:** o motor entregar o buffer que o fetch preencheu,
   em vez de ele ser copiado para a fila da thread de gravação.
3. **Shaders compilados fora do frame:** a compilação de pipelines passa para
   uma thread de fundo, para nenhum frame esperar por ela.
4. **Windows:** o backend Vulkan e o build são só Linux hoje.

---

# Roadmap (English)

The goal is **120+ fps at 4K with FSR**, everywhere in the game.

## Done

- The whole game runs: boot, videos, menus, cutscenes and gameplay, with
  audio, controller and saves.
- All SPU code translated to C.
- AO and lighting on the GPU.
- 2×/3× resolution scale.
- Water, particles and fragment depth correct.
- Brazilian Portuguese dub.
- Full screen with the right aspect.
- Main menu (Continue, New game, Load game, Options, Quit) and in-game
  options (F1).
- Performance work:
  - a frame in flight;
  - cheaper draws;
  - geometry resident in VRAM;
  - a Vulkan recording thread;
  - no GPU waits when textures are released.

## In progress

- **Rare mid-game freezes:** `play.sh` captures every thread's stack.
- **RSX thread per-draw cost:** in the heaviest views every busy thread is at
  84–94%.
- **SPU throughput:** it limits New Marais.
- **First shader build spikes.**

## Next

1. **FSR upscaling.** The route is still open: Vulkan + FSR 3.1 or D3D12 +
   FSR 4.1 through Proton.
2. **Zero-copy vertices** for the recording thread.
3. **Pipeline compilation off the frame**, on a background thread.
4. **Windows support.**
