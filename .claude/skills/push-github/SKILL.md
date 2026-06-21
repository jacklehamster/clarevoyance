---
description: Push current branch to GitHub when the built-in proxy lacks write access. Uses a stored PAT as fallback.
---

# push-github

Use this skill when `git push` fails with 403 (the Claude remote-session proxy is read-only).

## How it works

1. Look for a PAT in the environment: `$GITHUB_PAT`
2. If found, temporarily swap the remote URL to use it, push, then restore the original remote
3. Create a PR via the GitHub MCP if one doesn't already exist

## Steps

```bash
# 1. Check for token
if [ -z "$GITHUB_PAT" ]; then
  echo "GITHUB_PAT not set — ask user to provide a classic PAT with repo scope"
  exit 1
fi

# 2. Push using PAT
OWNER=jacklehamster
REPO=clarevoyance
BRANCH=$(git branch --show-current)
ORIGINAL_URL=$(git remote get-url origin)

git remote set-url origin "https://${OWNER}:${GITHUB_PAT}@github.com/${OWNER}/${REPO}.git"
git push -u origin "$BRANCH"
git remote set-url origin "$ORIGINAL_URL"
```

## Making GITHUB_PAT available

**On your local machine** — add to `~/.zshrc` or `~/.bashrc`:
```bash
export GITHUB_PAT=ghp_your_token_here
```

**In remote Claude Code sessions** — add `GITHUB_PAT` as an environment variable in your environment config at code.claude.com (Settings → Environments → your environment → Environment variables).

## Generating a new token

If the token is expired or revoked:
1. Go to github.com/settings/tokens → Generate new token (classic)
2. Select the `repo` scope
3. Set a short expiry (7 days is fine)
4. Update `GITHUB_PAT` in your shell profile / environment config
