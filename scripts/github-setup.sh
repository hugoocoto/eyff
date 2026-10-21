#!/usr/bin/env bash
# Set up the project's GitHub repo with gh. It asks about each setting,
# suggesting what this project expects (Enter takes it):
#
#   scripts/github-setup.sh [OWNER/REPO]
#
# - the repo itself, if it doesn't exist yet, and origin pointing to it
# - its description
# - private vulnerability reporting, where SECURITY.md sends reports
# - a read-only GITHUB_TOKEN by default: CI asks for more where it needs it
# - Dependabot alerts and security updates
# - main can't be force-pushed or deleted, and, if you want, only takes
#   commits that passed CI (its "ok" check): changes then go through pull
#   requests
# - only admins can create, move or delete v* tags, as each one is published
#   as a signed release
#
# Running it again updates what's there. It needs gh, logged in as an admin
# of the repo (gh auth login), and a terminal: it asks before changing
# anything.
set -euo pipefail
cd "$(dirname "$0")/.."

die() { echo "github-setup: $*" >&2; exit 1; }

{ : </dev/tty; } 2>/dev/null || die "it needs a terminal: it asks before changing anything"
command -v gh >/dev/null || die "it needs gh: https://cli.github.com"
gh auth status >/dev/null 2>&1 || die "gh is not logged in: gh auth login"

# ask VAR QUESTION SUGGESTION: the answer, with the suggestion filled in
ask() {
        local -n answer=$1
        # shellcheck disable=SC2034 # answer is the caller's variable
        read -rep "$2: " -i "$3" answer </dev/tty || die "cancelled"
}

# confirm QUESTION [n]: yes unless answered no, or no unless answered yes
confirm() {
        local ok default=${2:-y} hint='[Y/n]'
        [ "$default" = y ] || hint='[y/N]'
        read -rp "$1 $hint " ok </dev/tty || die "cancelled"
        case ${ok:-$default} in
        [yY]*) return 0 ;;
        *) return 1 ;;
        esac
}

# try WHAT COMMAND...: run it, and say how it went. A failure doesn't stop
# the rest: the summary at the end lists it.
failed=''
try() {
        local what=$1
        shift
        if "$@"; then
                echo "  done: $what"
        else
                echo "  failed: $what" >&2
                failed+="${failed:+, }$what"
        fi
}

# ruleset NAME JSON: make the ruleset NAME, whose fields are JSON (without
# its name), or update it if it exists
ruleset() {
        local id body="{\"name\": \"$1\", $2}"
        id=$(gh api "repos/$repo/rulesets" --jq ".[] | select(.name == \"$1\") | .id") || return
        if [ -n "$id" ]; then
                gh api --method PUT "repos/$repo/rulesets/$id" --input - --silent <<<"$body"
        else
                gh api --method POST "repos/$repo/rulesets" --input - --silent <<<"$body"
        fi
}

origin=$(git remote get-url origin 2>/dev/null || true)
slug=$(printf '%s' "$origin" | sed -nE 's#^.*github\.com[:/]+([^/]+/[^/]+)$#\1#p' | sed 's/\.git$//')
repo=${1:-${slug:-hugoocoto/eyff}}
ask repo "GitHub repo (owner/repo)" "$repo"
[[ $repo =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || die "'$repo' is not owner/repo"

desc=$(gh repo view "$repo" --json description --jq .description 2>/dev/null || true)
ask desc "Description" "${desc:-Explore Your F*cking Files}"

if ! gh repo view "$repo" >/dev/null 2>&1; then
        while :; do
                ask visibility "$repo doesn't exist: create it (public, private, or no)" public
                case $visibility in public | private | no) break ;; esac
        done
        [ "$visibility" != no ] || die "there's nothing to set up without the repo"
        gh repo create "$repo" "--$visibility" --description "$desc" >/dev/null || die "couldn't create $repo"
        echo "  done: created $repo"
fi

# origin: the repo, the way gh clones (https or ssh)
url=$(gh repo view "$repo" --json url --jq .url).git
[ "$(gh config get git_protocol 2>/dev/null)" != ssh ] || url=$(gh repo view "$repo" --json sshUrl --jq .sshUrl)
if [ -z "$origin" ]; then
        if confirm "Add $url as origin?"; then try "origin" git remote add origin "$url"; fi
elif [ "$slug" != "$repo" ]; then
        if confirm "origin is $origin: point it to $url instead?"; then try "origin" git remote set-url origin "$url"; fi
fi

visibility=$(gh repo view "$repo" --json visibility --jq .visibility)

if [ "$(gh repo view "$repo" --json description --jq .description)" != "$desc" ] &&
        confirm "Set its description to \"$desc\"?"; then
        try "description" gh repo edit "$repo" --description "$desc"
fi

if [ "$visibility" = PUBLIC ]; then
        if confirm "Turn on private vulnerability reporting, where SECURITY.md sends reports?"; then
                try "private vulnerability reporting" gh api --method PUT "repos/$repo/private-vulnerability-reporting" --silent
        fi
else
        echo "Private vulnerability reporting is only for public repos: SECURITY.md's email is the way to report until then."
fi

if confirm "Make the workflows' GITHUB_TOKEN read-only by default (CI asks for more where it needs it)?"; then
        try "read-only GITHUB_TOKEN" gh api --method PUT "repos/$repo/actions/permissions/workflow" \
                -f default_workflow_permissions=read -F can_approve_pull_request_reviews=false --silent
fi

if confirm "Turn on Dependabot alerts and security updates?"; then
        try "Dependabot alerts" gh api --method PUT "repos/$repo/vulnerability-alerts" --silent
        try "Dependabot security updates" gh api --method PUT "repos/$repo/automated-security-fixes" --silent
fi

# Rulesets: on private repos they need a paid plan
if confirm "Protect main: no force pushes, and it can't be deleted?"; then
        rules='{"type": "deletion"}, {"type": "non_fast_forward"}'
        # The "ok" job in ci.yml passes only if every other one did. 15368 is
        # GitHub Actions, so nothing else can pass it. The first push, which
        # makes main, has nothing to check yet.
        if confirm "Also only take commits on main that passed CI? Changes then go through pull requests" n; then
                rules+=', {"type": "required_status_checks", "parameters": {
                        "strict_required_status_checks_policy": false, "do_not_enforce_on_create": true,
                        "required_status_checks": [{"context": "ok", "integration_id": 15368}]}}'
        fi
        try "main's ruleset" ruleset main '"target": "branch", "enforcement": "active",
                "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
                "rules": ['"$rules"']'
fi

# 5 is the repository admin role
if confirm "Only let admins create, move or delete v* tags (each one is published as a signed release)?"; then
        try "release tags' ruleset" ruleset "release tags" '"target": "tag", "enforcement": "active",
                "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
                "rules": [{"type": "creation"}, {"type": "update"}, {"type": "deletion"}],
                "bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]'
fi

if ! git ls-remote --exit-code origin main >/dev/null 2>&1; then
        echo "main isn't on GitHub yet: commit, then git push -u origin main"
fi
[ -z "$failed" ] || die "failed: $failed (see above; run it again once fixed)"
echo "GitHub repo $repo is set up."
