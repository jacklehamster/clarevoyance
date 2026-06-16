# Clarevoyance

An action RPG built in C/C++ using SDL2 and OpenGL, targeting desktop, browser (WebAssembly/Emscripten), and Nintendo Switch.

## Overview

Clare, a young woman with clairvoyance since birth, races to prevent her King's assassination — and misreads everything along the way. The game's core mechanic mirrors her gift: players see translucent "shims" (ghost previews) of incoming attacks moments before they land.

## Docs

- [CLAUDE.md](CLAUDE.md) — architecture decisions, tech stack, coding conventions (read by AI agents)
- [docs/STORY.md](docs/STORY.md) — full narrative, characters, boss designs

## Structure

```
/src
  /engine       — core engine systems (renderer, camera, entity manager, collision)
  /game         — game-specific logic (combat, clairvoyance system, enemy AI)
  /levels       — level data, dungeon layouts, road definitions
/assets
  /sprites      — character and enemy billboard sprites
  /textures     — environment textures and tiles
  /audio        — music and sound effects
/docs
  STORY.md      — full narrative and character reference
```

## Build

### Desktop
```bash
make build    # → build/clarevoyance
make run      # build + open .app
```

### Browser (WebAssembly)
```bash
source ~/emsdk/emsdk_env.sh
make build-wasm   # → build/web/
make run-wasm     # build + serve locally
```

### Deploy to Cloudflare Pages
```bash
source ~/emsdk/emsdk_env.sh && make build-wasm
mkdir -p docs-web && cp build/web/* docs-web/
git add docs-web/ && git commit -m "Deploy: update WASM" && git push
```

Live at: https://clare.dobuki.net

## Status

Pre-production. See [CLAUDE.md](CLAUDE.md) for architecture decisions and [docs/STORY.md](docs/STORY.md) for the full narrative.
