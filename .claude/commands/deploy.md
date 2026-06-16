# Deploy to Cloudflare Pages

Builds the WebAssembly target locally and publishes it to Cloudflare Pages (clare.dobuki.net).

## Steps

```bash
# 1. Build WASM
source ~/emsdk/emsdk_env.sh && make build-wasm

# 2. Copy output to tracked folder
mkdir -p docs-web && cp build/web/* docs-web/

# 3. Commit and push — Cloudflare auto-deploys on push to main
git add docs-web/
git commit -m "Deploy: update WASM build"
git push origin main
```

## Notes

- Cloudflare Pages project: `clarevoyance` (account: Vincent, ID: `1fe1ef92444f52ef8d7ff09c175d034e`)
- Live URLs: https://clare.dobuki.net and https://clarevoyance.pages.dev
- Build command on CF is empty — we pre-build locally because Emscripten isn't available on CF build machines
- Output directory on CF: `docs-web/`
- Deployment status: https://dash.cloudflare.com/1fe1ef92444f52ef8d7ff09c175d034e/pages/view/clarevoyance

## Cloudflare MCP

The `cloudflare-api` MCP is configured globally in `~/.claude/settings.json`. Claude can manage
the Pages project directly using `mcp__plugin_cloudflare_cloudflare-api__execute`.

Account IDs:
- Vincent: `1fe1ef92444f52ef8d7ff09c175d034e`
- dobuki.net zone: `849149529f0b407bc3215d3f0986d08d`
