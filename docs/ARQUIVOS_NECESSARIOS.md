# Arquivos necessários

Tudo vem do **seu** PS3. Nada disto está no repositório, e nada que o build
produz a partir desses arquivos pode ser distribuído: o código traduzido é o
código do jogo.

## 1. O disco

**inFAMOUS 2, BCUS98125 (EUA), versão 01.00.** O port é feito para os
endereços desse executável. O `scripts/lift.sh` confere o SHA-256 e avisa se
for outro.

1. Faça o dump do seu disco num PS3 seu. Há guias para isso; este projeto não
   ajuda a obter o jogo.
2. Coloque a pasta do disco de modo que `disc/PS3_GAME/` fique na raiz deste
   repositório. O `disc/` é ignorado pelo git.

| Caminho | Para quê |
|---|---|
| `disc/PS3_GAME/PARAM.SFO` | o título (BCUS98125) que o port informa ao jogo |
| `disc/PS3_GAME/USRDIR/EBOOT.BIN` | o executável, ainda criptografado |
| `disc/PS3_GAME/USRDIR/cache/psarc/*.psarc` | o conteúdo do jogo, inclusive `brazilian.psarc` e `brazilian_text.psarc` (dublagem e textos em PT-BR) |
| o resto de `disc/PS3_GAME/USRDIR/` | vídeos, legendas, dados de instalação |

## 2. O executável descriptografado

Descriptografe `disc/PS3_GAME/USRDIR/EBOOT.BIN` para
`disc/PS3_GAME/USRDIR/EBOOT.elf`, por exemplo no RPCS3 (Utilities → Decrypt
PS3 Binaries). É dele que o `scripts/lift.sh` tira o código PPU e as imagens
SPU.

## 3. O firmware (`dev_flash`)

O jogo lê alguns arquivos de `/dev_flash`. O port usa o firmware que o RPCS3
instalou a partir do PUP oficial:

- Flatpak: `~/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash`
- nativo: `~/.config/rpcs3/dev_flash`

Para outro lugar: `PS3_DEV_FLASH=<pasta>`.

## 4. O PC

- **Linux x86-64.** Testado no Fedora 44.
- **CPU:**
  - O código PPU traduzido usa x86-64-v3 (AVX2, FMA).
  - O código SPU usa x86-64-v4 (AVX-512).
  - CPU sem AVX-512: `PORT_SPU_MARCH=x86-64-v3`.
  - CPU sem AVX2: `PORT_PPU_MARCH=` (vazio).
- **GPU** com Vulkan e uma fila gráfica que também faz compute.
- **Pacotes no Fedora** (adapte para a sua distro): `gcc gcc-c++ clang cmake
  ninja-build python3 SDL2-devel vulkan-headers vulkan-loader-devel
  glslang-devel`.
- **~14 GB de RAM livres** para compilar e **~3 GB de disco** para o código
  traduzido e os builds.

## Depois

```sh
scripts/setup.sh
scripts/lift.sh
scripts/build.sh
work/play.sh
```

Na primeira vez o jogo instala dados em `hdd0/` (ao lado de `disc/`), como
faria no console. Os saves ficam em
`gamedata/dev_hdd0/home/00000001/savedata/`.

---

# Required files (English)

- **The disc:** inFAMOUS 2 **BCUS98125 (US), version 01.00**, dumped from your
  own PS3, with `disc/PS3_GAME/` at the repository root.
- **The decrypted executable:** `disc/PS3_GAME/USRDIR/EBOOT.BIN` decrypted to
  `EBOOT.elf` next to it (RPCS3: Utilities → Decrypt PS3 Binaries).
  `scripts/lift.sh` checks its SHA-256.
- **The firmware:** RPCS3's `dev_flash`, installed from the official PUP
  (Flatpak or native path above, or `PS3_DEV_FLASH=<dir>`).
- **The PC:**
  - x86-64 Linux.
  - AVX2. The SPU code uses AVX-512: `PORT_SPU_MARCH=x86-64-v3` without it.
  - A Vulkan GPU.
  - ~14 GB of free RAM to build.

Nothing built from these files may be shared: it is the game's code,
translated.

## Levar os arquivos para outro PC

O repositório é público e não carrega nada do jogo, então o `disc/` (com o
`EBOOT.elf`) não vai pelo git. Copie direto de uma máquina sua para a outra:

```bash
# na máquina que já tem o disc/, com o clone do repositório já feito na outra
rsync -aP disc/ usuario@outro-pc:~/ps3infamou/disc/
```

Depois, na outra máquina: `git pull` e o `scripts/lift.sh` como de costume.
