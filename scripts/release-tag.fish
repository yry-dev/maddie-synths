#!/usr/bin/env fish
#
# Cut a dated release tag from today's date and push it. Pushing the tag is what
# triggers .github/workflows/release.yml, which builds every Rack platform and
# every firmware and publishes a GitHub release.
#
#   scripts/release-tag.fish             -> v2608.15       stable, also moves `latest`
#   scripts/release-tag.fish --next      -> v2608.15-next  pre-release, leaves `latest` alone
#   scripts/release-tag.fish --dry-run   -> print what it would do, touch nothing
#
# The tag carries only the MINOR.REVISION half of the Rack plugin version; CI
# prepends RACK_MAJOR ("2"), so v2608.15 publishes as plugin version 2.2608.15.
# See "Versioning / releases" in CLAUDE.md.

set -l repo_root (cd (dirname (status -f)); pwd)/..
set -l remote origin

set -l next false
set -l dry_run false
set -l assume_yes false
set -l force false

for arg in $argv
    switch $arg
        case --next
            set next true
        case --dry-run -n
            set dry_run true
        case --yes -y
            set assume_yes true
        case --force -f
            set force true
        case --help -h
            echo "Usage: scripts/release-tag.fish [--next] [--dry-run] [--yes] [--force]"
            echo ""
            echo "  --next     tag a pre-release (vYYMM.DD-next) instead of a stable release"
            echo "  --dry-run  show the tag and the checks, then stop"
            echo "  --yes      skip the confirmation prompt"
            echo "  --force    move an existing tag of the same name (re-releases that day)"
            exit 0
        case '*'
            echo "release-tag: unknown argument '$arg' (try --help)" >&2
            exit 1
    end
end

# vYYMM.DD -- e.g. 2026-08-15 -> v2608.15
set -l datestamp (date +'%y%m.%d')
set -l tag "v$datestamp"
set -l plugin_version "2.$datestamp"
if test "$next" = true
    set tag "$tag-next"
    set plugin_version "$plugin_version-next.<sha7>"
end

set -l head_sha (git -C $repo_root rev-parse --short HEAD)
set -l branch (git -C $repo_root rev-parse --abbrev-ref HEAD)

# One dated tag per day is all the vYYMM.DD format can express, so a collision
# means a release already went out today. Refuse rather than silently reuse it.
for scope in local remote
    set -l exists false
    if test $scope = local
        git -C $repo_root rev-parse -q --verify "refs/tags/$tag" >/dev/null; and set exists true
    else
        git -C $repo_root ls-remote --exit-code --tags $remote "refs/tags/$tag" >/dev/null 2>&1; and set exists true
    end
    if test "$exists" = true; and test "$force" != true
        echo "release-tag: $tag already exists ($scope)." >&2
        echo "  A release already went out today. Use --next for a pre-release," >&2
        echo "  or --force to move $tag to $head_sha and re-release." >&2
        exit 1
    end
end

# Advisory checks -- none of these are fatal, but all of them change what ships.
# Count elements rather than `test -n (...)`: an empty command substitution
# expands to zero arguments, leaving `test -n`, which fish reads as the
# non-empty string "-n" and reports every clean tree as dirty.
set -l dirty (git -C $repo_root status --porcelain)
if test (count $dirty) -gt 0
    echo "release-tag: warning -- working tree is dirty; uncommitted changes will NOT be in the release."
end
if test "$branch" != main
    echo "release-tag: warning -- on branch '$branch', not main."
end
if git -C $repo_root rev-parse -q --verify "$remote/main" >/dev/null
    set -l ahead (git -C $repo_root rev-list --count "$remote/main..HEAD")
    if test "$ahead" -gt 0
        echo "release-tag: warning -- HEAD is $ahead commit(s) ahead of $remote/main; the branch itself won't be pushed."
    end
end

echo ""
echo "  tag             $tag"
echo "  commit          $head_sha ($branch)"
echo "  plugin version  $plugin_version"
if test "$next" = true
    echo "  release         pre-release; rolling 'latest' stays put"
else
    echo "  release         stable; rolling 'latest' moves to this commit"
end
echo ""

if test "$dry_run" = true
    echo "[dry run] nothing created or pushed."
    exit 0
end

if test "$assume_yes" != true
    read -l -P "Push $tag to $remote and publish a release? [y/N] " reply
    if not string match -qr '^[Yy]$' -- "$reply"
        echo "Aborted."
        exit 1
    end
end

set -l tag_flags -a
set -l push_flags
if test "$force" = true
    set -a tag_flags -f
    set -a push_flags --force
end

git -C $repo_root tag $tag_flags "$tag" -m "Release $tag"
or exit 1
git -C $repo_root push $push_flags $remote "refs/tags/$tag"
or exit 1

echo ""
echo "Pushed $tag. Watch the build:"
echo "  gh run watch (gh run list --workflow=release.yml --limit 1 --json databaseId --jq '.[0].databaseId')"
