#!/usr/bin/env bash
#
#
# Copyright (c) 2015-2026 unfacd works
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#

# dev/scripts/build/build_install_verify.sh — one pathway per invocation: local
# build, docker deploy, remote deploy, or mirror.
#
# Modelled on ufsrvcorelib/dev/scripts/build_install_verify.sh, obeying the
# program-instead-of-library differences:
#
#   * ufsrvstun has no CMake install() rules and no CPack. A local run builds and
#     verifies only: the binary stays in build/<preset>/ufsrvstun and nothing
#     is installed on this host.
#   * there is no install_verification target. Verification is the binary's own
#     --version short-circuit (src/main.c honours it before config, DB and
#     credentials) plus dynamic-linkage resolution, then ctest when --test
#     configured the harness in.
#   * the deployed artefact is produced by the ufsrvstun_src role on the target,
#     not here: the remote and docker pathways hand the work to ansible and stop.
#
# Pathways — exactly one, and the script stops when it finishes:
#   local            configure -> build -> verify (default)
#   --docker-deploy  ufsrvstun_build.yml --tags ufsrvstun_src, docker inventory
#   --remote-deploy  ufsrvstun_build.yml --tags ufsrvstun_src, hosts_prod
#   --mirror         t2gm.sh only; no relation to building or configuring
#   --helgrind-docker
#                    run the deployed ufsrvstun binary under Valgrind Helgrind in
#                    the docker container; the instance must already be stopped
#   --is-up-docker   docker: report whether the container is running and its
#                    instance unit is active; exit 0 if up, non-zero otherwise.
#                    Reads state only — starts nothing, changes nothing
#
# --origin-push runs first, before the pathway does anything: both deploy
# pathways build what origin holds, so the push is what makes them mean
# anything. --tag requires --origin-push.
#
# The preset is dev|release and defaults to dev. It selects the local build
# directory and, for the deploy pathways, the profile the role turns back into a
# preset: dev -> ufsrvstun_src_cmake_profile=dev, release -> prod (the role's name
# for the release preset).
#
# Tags carry MAJOR.MINOR.PATCH only. UFSRV_INTERNAL marks a gate, not a release,
# so it is printed for context and never tagged.
#
# Usage:
#   dev/scripts/build/build_install_verify.sh [dev|release] [options]
#
#   $0                                  # local: configure -> build -> verify
#   $0 release --test                   # local: release preset + ctest
#   $0 --origin-push --docker-deploy    # docker: push, then ufsrvstun_src on docker
#   $0 --remote-deploy                  # VPS: hosts_prod / zone1-ufsrv-stun
#   $0 --mirror                         # GitHub mirror only
#
# Exit status: 0 on success; non-zero if any step fails (set -euo pipefail).
set -euo pipefail

# ── Defaults ────────────────────────────────────────────────────────────────

PRESET="dev"
JOBS="$(nproc)"
JOBS_SET=0
FRESH=0
RUN_TESTS=0
DRY_RUN=0
# Companions must resolve from the system install; the pre-flight guard below
# rejects a build directory pinned to any other prefix. An isolation experiment
# left in place silently shadows this one, so a later install is ignored and the
# compile error looks unrelated to its cause.
COMPANION_PREFIX="${COMPANION_PREFIX:-/usr/local}"
COMPANION_PREFIX="${COMPANION_PREFIX%/}"

BYPASS_LOCAL=0
ORIGIN_PUSH=0
TAG=0
TAG_SUFFIX=""
PATHWAY="local"
MIRROR_OPTS=""
DEPLOY_TAG="ufsrvstun_src"

ANSIBLE_REPO="${ANSIBLE_REPO:-/home/devops/repos/ansible}"
ANSIBLE_VAULT_FILE="${ANSIBLE_VAULT_FILE:-vault.txt}"
ANSIBLE_PROD_INVENTORY="${ANSIBLE_PROD_INVENTORY:-hosts_prod}"
ANSIBLE_REMOTE_PLAYBOOK="${ANSIBLE_REMOTE_PLAYBOOK:-ufsrvstun_build.yml}"
ANSIBLE_REMOTE_LIMIT="${ANSIBLE_REMOTE_LIMIT:-zone1-ufsrv-stun}"
ANSIBLE_DOCKER_INVENTORY="${ANSIBLE_DOCKER_INVENTORY:-/home/devops/repos/docker/ansible_driven_dockerisation/inventory_ufsrv_hosts_flat_docker.ini}"
ANSIBLE_DOCKER_PLAYBOOK="${ANSIBLE_DOCKER_PLAYBOOK:-ufsrvstun_build.yml}"
ANSIBLE_DOCKER_LIMIT="${ANSIBLE_DOCKER_LIMIT:-ufsrvstun}"
ANSIBLE_DOCKER_CONTAINER="${ANSIBLE_DOCKER_CONTAINER:-ufsrvstun}"
ANSIBLE_DOCKER_INSTANCE="${ANSIBLE_DOCKER_INSTANCE:-1}"
# The service name follows the container name by default; kept separate so a
# container renamed for operations does not have to rename its unit.
ANSIBLE_DOCKER_SERVICE="${ANSIBLE_DOCKER_SERVICE:-$ANSIBLE_DOCKER_CONTAINER}"

HELGRIND_CONTAINER="${HELGRIND_CONTAINER:-$ANSIBLE_DOCKER_CONTAINER}"
HELGRIND_ROOT="${HELGRIND_ROOT:-/opt/ufsrv/ufsrvstun}"
HELGRIND_BIN="${HELGRIND_BIN:-$HELGRIND_ROOT/bin/ufsrvstun}"
HELGRIND_WORKDIR="${HELGRIND_WORKDIR:-$HELGRIND_ROOT/etc}"
HELGRIND_CONFIG="${HELGRIND_CONFIG:-ufsrvstun-1.lua}"
HELGRIND_INSTANCE="${HELGRIND_INSTANCE:-1}"
HELGRIND_SUPPRESSIONS="${HELGRIND_SUPPRESSIONS:-$HELGRIND_ROOT/etc/valgrind_suppressions.supp}"
HELGRIND_HOST_LOG="${HELGRIND_HOST_LOG:-$PWD/helgrind.log}"

VERSION_HEADER="${REPO_VERSION_HEADER:-src/include/version.h}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

helgrind_verdict() {
    _log="$1"
    if [ ! -s "$_log" ]; then
        printf '   FAIL: no Helgrind output captured\n'
        return 1
    fi
    _abort=$(grep -m1 -iE '^==[0-9]+==.*(aborting|asan cannot proceed)|^valgrind: ' "$_log")
    if [ -n "$_abort" ]; then
        printf '   FAIL: guest did not run — %s\n' "$(printf '%s' "$_abort" | sed 's/^==[0-9]*==[[:space:]]*//')"
        return 1
    fi
    _summary=$(grep -E '^==[0-9]+==.*ERROR SUMMARY: [0-9]+ errors?' "$_log" | tail -1 | sed 's/^==[0-9]*==[[:space:]]*//')
    if [ -z "$_summary" ]; then
        printf '   FAIL: no ERROR SUMMARY — the run did not complete\n'
        return 1
    fi
    if printf '%s' "$_summary" | grep -q 'ERROR SUMMARY: 0 errors'; then
        printf '   PASS: %s\n' "$_summary"
        return 0
    fi
    printf '   FAIL: %s\n' "$_summary"
    return 1
}

usage() {
    cat <<EOF
Usage: $0 [dev|release] [options]

One pathway per invocation. The script runs that pathway and stops.

Pathways:
  (default)         local: configure -> build -> verify. Nothing is installed;
                      the binary stays in build/<preset>/ufsrvstun
  --docker-deploy[=TAG]   docker: ufsrvstun_build.yml against the docker
                      inventory, for the ufsrvstun container
  --remote-deploy[=TAG]   VPS: ufsrvstun_build.yml against hosts_prod, for
                      zone1-ufsrv-stun
                      TAG is passed verbatim to ansible's --tags, default
                      ufsrvstun_src; =recompile_src rebuilds and installs the binary
                      only
  --mirror          mirror only: t2gm.sh, no configure and no build
  --mirror=OPTS     same, with explicit comma-separated t2gm.sh options
  --helgrind-docker run the deployed ufsrvstun binary under Valgrind Helgrind
                      inside the docker container; the instance must be stopped
                      first, and is left as found
  --is-up-docker    docker: is the container running and its instance unit
                      active? Exit 0 when up, non-zero when not. Read-only:
                      starts nothing and changes nothing

Local pathway:
  PRESET            positional: dev|release (default: dev)
  --preset NAME     the same, explicitly
  --jobs N          Parallel build jobs (default: \$(nproc))
  --fresh           Fresh configure (cmake --fresh): full clean rebuild
  --test            Build the test harness and run ctest after verification

Git:
  --origin-push     Push the current branch to origin first, before the pathway
  --tag             Tag HEAD as vX.Y.Z and push it — requires --origin-push
  --tag=SUFFIX      Tag HEAD as vX.Y.Z_SUFFIX and push it — requires --origin-push
  --bypass-local    Continue despite uncommitted local changes

Any pathway:
  --dry-run         Print commands without executing them
  -h, --help        Show this help

Environment:
  ANSIBLE_REPO              Path to the ansible repo (default: /home/devops/repos/ansible)
  ANSIBLE_VAULT_FILE        Vault password file (default: vault.txt)
  ANSIBLE_PROD_INVENTORY    Prod inventory (default: hosts_prod)
  ANSIBLE_REMOTE_PLAYBOOK   Prod playbook (default: ufsrvstun_build.yml)
  ANSIBLE_REMOTE_LIMIT      Prod limit (default: zone1-ufsrv-stun)
  ANSIBLE_DOCKER_INVENTORY  Docker inventory (default: docker/ansible_driven_dockerisation/inventory_ufsrv_hosts_flat_docker.ini)
  ANSIBLE_DOCKER_PLAYBOOK   Docker playbook (default: ufsrvstun_build.yml)
  ANSIBLE_DOCKER_LIMIT      Docker inventory group (default: ufsrvstun)
  ANSIBLE_DOCKER_CONTAINER  Container checked before deploying (default: ufsrvstun)
  HELGRIND_CONTAINER        Container for --helgrind-docker (default: $ANSIBLE_DOCKER_CONTAINER)
  HELGRIND_INSTANCE         systemd instance to run (default: 1)
  HELGRIND_HOST_LOG         Host-side log path (default: ./helgrind.log)
EOF
}

# ── Argument parsing ────────────────────────────────────────────────────────

set_pathway() {
    if [ "$PATHWAY" != "local" ]; then
        die "one pathway per invocation — already '${PATHWAY}', cannot also '${1}'"
    fi
    PATHWAY="$1"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --preset)           PRESET="$2"; shift 2 ;;
        --jobs)             JOBS="$2"; JOBS_SET=1; shift 2 ;;
        --fresh)            FRESH=1; shift ;;
        --test)             RUN_TESTS=1; shift ;;
        --dry-run)          DRY_RUN=1; shift ;;
        --origin-push)      ORIGIN_PUSH=1; shift ;;
        --tag)              TAG=1; TAG_SUFFIX=""; shift ;;
        --tag=*)            TAG=1; TAG_SUFFIX="${1#--tag=}"; shift ;;
        --bypass-local)     BYPASS_LOCAL=1; shift ;;
        --docker-deploy)    set_pathway docker; DEPLOY_TAG="ufsrvstun_src"; shift ;;
        --docker-deploy=*)  set_pathway docker; DEPLOY_TAG="${1#--docker-deploy=}"; shift ;;
        --remote-deploy)    set_pathway remote; DEPLOY_TAG="ufsrvstun_src"; shift ;;
        --remote-deploy=*)  set_pathway remote; DEPLOY_TAG="${1#--remote-deploy=}"; shift ;;
        --mirror)           set_pathway mirror; MIRROR_OPTS="cleanup_repo_root"; shift ;;
        --mirror=*)         set_pathway mirror; MIRROR_OPTS="${1#--mirror=}"; shift ;;
        --helgrind-docker)  set_pathway helgrind; shift ;;
        --is-up-docker)     set_pathway isup; shift ;;
        -h|--help)          usage; exit 0 ;;
        --*)                usage >&2; die "unknown option '$1'" ;;
        *)                  PRESET="$1"; shift ;;
    esac
done

case "$PRESET" in
    dev)     PROFILE="dev";  BUILD_DIR="build/dev" ;;
    release) PROFILE="prod"; BUILD_DIR="build/release" ;;
    *)       die "preset must be dev or release, not '${PRESET}'" ;;
esac

if [ "$TAG" -eq 1 ] && [ "$ORIGIN_PUSH" -eq 0 ]; then
    die "--tag requires --origin-push"
fi

if [ "$PATHWAY" != "local" ]; then
    [ "$RUN_TESTS" -eq 1 ] && die "--test is local-only"
    [ "$FRESH" -eq 1 ] && die "--fresh is local-only"
    [ "$JOBS_SET" -eq 1 ] && die "--jobs is local-only"
fi

# --is-up-docker reports state and nothing else, so a push or a tag alongside it
# would be a side effect the switch promises not to have.
if [ "$PATHWAY" = "isup" ] && { [ "$ORIGIN_PUSH" -eq 1 ] || [ "$TAG" -eq 1 ]; }; then
    die "--is-up-docker is read-only: not combinable with --origin-push or --tag"
fi

[ -n "$DEPLOY_TAG" ] || die "deploy tag must not be empty"

BINARY="${BUILD_DIR}/ufsrvstun"

# ── Version (read once, for the banner and for the tag) ─────────────────────
#
# Hoisted out of the tag branch: the internal counter is worth printing whether
# or not this run tags anything, and only the tag needs the release triple.

MAJOR=""; MINOR=""; PATCH=""; INTERNAL=""
if [ -f "$VERSION_HEADER" ]; then
    MAJOR=$(sed -n 's/^#define[[:space:]]*[A-Za-z_]*_MAJOR[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$VERSION_HEADER" | head -1)
    MINOR=$(sed -n 's/^#define[[:space:]]*[A-Za-z_]*_MINOR[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$VERSION_HEADER" | head -1)
    PATCH=$(sed -n 's/^#define[[:space:]]*[A-Za-z_]*_PATCH[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$VERSION_HEADER" | head -1)
    INTERNAL=$(sed -n 's/^#define[[:space:]]*[A-Za-z_]*_INTERNAL[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$VERSION_HEADER" | head -1)
fi

if [ -n "$MAJOR" ] && [ -n "$MINOR" ] && [ -n "$PATCH" ]; then
    RELEASE="${MAJOR}.${MINOR}.${PATCH}"
else
    RELEASE=""
fi
if [ -n "$RELEASE" ] && [ -n "$INTERNAL" ]; then
    VERSION_DESC="${RELEASE} (internal ${INTERNAL})"
elif [ -n "$RELEASE" ]; then
    VERSION_DESC="${RELEASE} (internal: not declared)"
else
    VERSION_DESC="unknown"
fi

# ── Helpers ─────────────────────────────────────────────────────────────────

step() { printf '\n==> %s\n' "$*"; }

# report_linkage <binary> — how each companion library is linked.
#
# `ldd | grep 'not found'` only fires when a shared library is missing. A
# companion that was absorbed as a static archive is invisible there, and both
# flavours resolve identically once loaded — so the flavour has to be read from
# DT_NEEDED, with an embedded-symbol probe for the case where it is not listed.
report_linkage() {
    _rl_bin="$1"
    if [ ! -f "$_rl_bin" ]; then
        printf '   companion linkage: no binary at %s\n' "$_rl_bin"
        return 0
    fi
    _rl_needed="$(readelf -d "$_rl_bin" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')"
    printf '   companion linkage (%s):\n' "$_rl_bin"
    for _rl_spec in uflib:BufferDescriptorInit ufsrvcorelib:ConfFileDestroy \
                    ufnetcorelib:BackendDirectoryContactTokenGet ufsrvrxlib:UfLoggerCreate; do
        _rl_lib="${_rl_spec%%:*}"
        _rl_probe="${_rl_spec#*:}"
        _rl_dso="$(printf '%s\n' "$_rl_needed" | grep -E "^lib${_rl_lib}\.so" | head -1 || true)"
        if [ -n "$_rl_dso" ]; then
            printf '     %-14s shared   (%s)\n' "$_rl_lib" "$_rl_dso"
        elif nm "$_rl_bin" 2>/dev/null | grep " T ${_rl_probe}$" >/dev/null; then
            printf '     %-14s static   (archive objects inside the binary)\n' "$_rl_lib"
        else
            printf '     %-14s not linked\n' "$_rl_lib"
        fi
    done
}
run() {
    printf '+ %s\n' "$*"
    if [ "$DRY_RUN" -eq 0 ]; then
        "$@"
    fi
}

if [ "$TAG" -eq 0 ]; then
    TAG_DESC="off"
elif [ -n "$TAG_SUFFIX" ]; then
    TAG_DESC="vX.Y.Z_${TAG_SUFFIX}"
else
    TAG_DESC="vX.Y.Z"
fi

printf '==> pathway=%s preset=%s profile=%s push=%s tag=%s bypass-local=%s dry-run=%s\n' \
    "$PATHWAY" "$PRESET" "$PROFILE" \
    "$([ "$ORIGIN_PUSH" -eq 1 ] && echo yes || echo no)" \
    "$TAG_DESC" \
    "$([ "$BYPASS_LOCAL" -eq 1 ] && echo yes || echo no)" \
    "$([ "$DRY_RUN" -eq 1 ] && echo yes || echo no)"

printf '==> version: %s   source: %s\n' "$VERSION_DESC" \
    "$([ -f "$VERSION_HEADER" ] && echo "$VERSION_HEADER" || echo 'not found')"

case "$PATHWAY" in
    local)
        printf '==> build: %s jobs=%s tests=%s fresh=%s\n' "$BUILD_DIR" "$JOBS" \
            "$([ "$RUN_TESTS" -eq 1 ] && echo yes || echo no)" \
            "$([ "$FRESH" -eq 1 ] && echo yes || echo no)" ;;
    docker)
        printf '==> target: %s (docker inventory), tags=%s\n' "$ANSIBLE_DOCKER_LIMIT" "$DEPLOY_TAG" ;;
    remote)
        printf '==> target: %s (hosts_prod), tags=%s\n' "$ANSIBLE_REMOTE_LIMIT" "$DEPLOY_TAG" ;;
    mirror)
        printf '==> mirror options: %s\n' "$MIRROR_OPTS" ;;
    helgrind)
        printf '==> target: %s (docker), log=%s\n' "$HELGRIND_CONTAINER" "$HELGRIND_HOST_LOG" ;;
    isup)
        printf '==> target: %s (docker), instance=%s\n' "$ANSIBLE_DOCKER_CONTAINER" "$ANSIBLE_DOCKER_INSTANCE" ;;
esac

# ── Pre-flight: reject uncommitted local changes ─────────────────────────────

# Helgrind runs the already-deployed container binary; the checkout state is
# irrelevant, so this pathway is exempt from the clean-tree guard.
if [ "$BYPASS_LOCAL" -eq 0 ] && [ "$PATHWAY" != "helgrind" ] && [ "$PATHWAY" != "isup" ] && [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    echo "error: uncommitted local changes present" >&2
    echo "  Commit or stash them first, or re-run with --bypass-local to continue." >&2
    exit 1
fi

# ── Pre-flight: reject a build directory pinned to a scratch prefix ──────────

# CMake caches each package's *_DIR for the life of the build directory. A
# prefix left behind by an isolation experiment keeps winning over the system
# install, so a later companion install is ignored and the compile error looks
# unrelated to its cause. --fresh clears the cache and is the sanctioned way out.
if [ "$PATHWAY" = "local" ] && [ "$FRESH" -eq 0 ] && [ -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    pinned=""
    for _lib in uflib ufsrvcorelib ufnetcorelib ufsrvclilib ufsrvrxlib; do
        _value=$(sed -n "s/^${_lib}_DIR:[^=]*=//p" "${BUILD_DIR}/CMakeCache.txt" | head -1)
        case "$_value" in
            ""|*NOTFOUND*) continue ;;
            "$COMPANION_PREFIX"|"$COMPANION_PREFIX"/*) continue ;;
        esac
        pinned="${pinned}    ${_lib}_DIR=${_value}"$'\n'
    done
    if [ -n "$pinned" ]; then
        printf 'error: %s is pinned to a companion install outside %s:\n' "$BUILD_DIR" "$COMPANION_PREFIX" >&2
        printf '%s' "$pinned" >&2
        printf '  Re-run with --fresh to reconfigure against %s,\n' "$COMPANION_PREFIX" >&2
        printf '  or set COMPANION_PREFIX to the prefix this build is meant to use.\n' >&2
        exit 1
    fi
fi

# ── Origin push (always first) ──────────────────────────────────────────────

if [ "$ORIGIN_PUSH" -eq 1 ]; then
    branch=$(git rev-parse --abbrev-ref HEAD)
    if [ "$TAG" -eq 1 ]; then
        [ -n "$RELEASE" ] || die "cannot derive MAJOR.MINOR.PATCH from ${VERSION_HEADER} to tag"
        tag="v${RELEASE}"
        if [ -n "$TAG_SUFFIX" ]; then
            tag="${tag}_${TAG_SUFFIX}"
        fi
        step "Push branch + tag ($tag)"
        echo "   branch=$branch tag=$tag release=$RELEASE internal=${INTERNAL:-not declared}"

        if git rev-parse -q --verify "refs/tags/$tag" >/dev/null 2>&1; then
            echo "   (tag $tag already exists — reusing)"
        else
            run git tag -a "$tag" -m "$tag"
        fi
        run git push origin "$branch" "$tag"
    else
        step "Push branch"
        echo "   branch=$branch (no tag — pass --tag or --tag=<suffix> to tag)"
        run git push origin "$branch"
    fi
fi

# ── Pathway: local ──────────────────────────────────────────────────────────

if [ "$PATHWAY" = "local" ]; then

    step "Configure (preset=$PRESET)"
    # Tests are opt-in and stated either way: ctest has nothing to run unless the
    # harness was configured in, and leaving the flag off would let a build
    # directory whose cache says ON keep building them.
    if [ "$RUN_TESTS" -eq 1 ]; then
        tests_arg=(-DUFSRV_ENABLE_TESTS=ON)
    else
        tests_arg=(-DUFSRV_ENABLE_TESTS=OFF)
    fi
    if [ "$FRESH" -eq 1 ]; then
        run cmake --fresh --preset "$PRESET" "${tests_arg[@]}"
    else
        run cmake --preset "$PRESET" "${tests_arg[@]}"
    fi

    step "Build ($JOBS jobs)"
    run cmake --build "$BUILD_DIR" -j"$JOBS"

    step "Verify (--version, linkage)"
    # The program-shaped check: --version is honoured before config, DB and
    # credentials are consulted, so it proves the binary runs without needing a
    # deployment to run against.
    if [ "$DRY_RUN" -eq 0 ]; then
        if ! out="$("$BINARY" --version 2>&1)"; then
            die "$BINARY --version failed"
        fi
        printf '   %s\n' "$out"
        if [ -n "$RELEASE" ] && ! printf '%s' "$out" | grep -qF "$RELEASE"; then
            die "$BINARY --version reported '$out' — expected $RELEASE from $VERSION_HEADER"
        fi
        if missing=$(ldd "$BINARY" 2>/dev/null | grep 'not found' || true); [ -n "$missing" ]; then
            printf '%s\n' "$missing" >&2
            die "unresolved shared libraries in $BINARY"
        fi
        report_linkage "$BINARY"
    else
        printf '+ %s --version\n+ ldd %s\n+ readelf -d %s (companion linkage)\n' "$BINARY" "$BINARY" "$BINARY"
    fi

    if [ "$RUN_TESTS" -eq 1 ]; then
        step "Verify — ctest"
        run ctest --test-dir "$BUILD_DIR" --output-on-failure
    fi

fi

# ── Pathway: remote / docker deploy ─────────────────────────────────────────
#
# Both run the same role tag; the destination is decided by the playbook, which
# sets ufsrvstun_provision_target and finalises through ufsrvstun_src_vps.yml or
# ufsrvstun_src_docker.yml. The preset crosses as the role's cmake profile.

run_deploy() {
    local inventory="$1" limit="$2" playbook="$3"

    [ -f "$ANSIBLE_REPO/$playbook" ] || die "playbook not found: $ANSIBLE_REPO/$playbook"
    if [ ! -f "$inventory" ] && [ ! -f "$ANSIBLE_REPO/$inventory" ]; then
        die "inventory not found: $inventory"
    fi

    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+ (cd %s && ansible-playbook -i %s -l %s %s --vault-password-file=%s --tags %s -e ufsrvstun_src_cmake_profile=%s)\n' \
            "$ANSIBLE_REPO" "$inventory" "$limit" "$playbook" "$ANSIBLE_VAULT_FILE" "$DEPLOY_TAG" "$PROFILE"
        return 0
    fi

    (
        cd "$ANSIBLE_REPO"
        ansible-playbook -i "$inventory" -l "$limit" "$playbook" \
            --vault-password-file="$ANSIBLE_VAULT_FILE" \
            --tags "$DEPLOY_TAG" \
            -e "ufsrvstun_src_cmake_profile=${PROFILE}"
    )
}

if [ "$PATHWAY" = "docker" ]; then
    step "Docker deploy ($ANSIBLE_DOCKER_LIMIT -> $ANSIBLE_DOCKER_PLAYBOOK)"

    # Fail fast on a stopped container: the playbook's first pre_task runs
    # `docker exec` against it, which would report a Docker error instead of
    # naming the target. A missing container fails the same check.
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+ docker inspect -f %s %s (must report true)\n' \
            "'{{.State.Running}}'" "$ANSIBLE_DOCKER_CONTAINER"
    elif ! docker inspect -f '{{.State.Running}}' "$ANSIBLE_DOCKER_CONTAINER" 2>/dev/null | grep -qx true; then
        die "container '$ANSIBLE_DOCKER_CONTAINER' is not running (start it with: docker start $ANSIBLE_DOCKER_CONTAINER)"
    fi

    run_deploy "$ANSIBLE_DOCKER_INVENTORY" "$ANSIBLE_DOCKER_LIMIT" "$ANSIBLE_DOCKER_PLAYBOOK"
fi

# ── Pathway: container state (read-only) ────────────────────────────────────

if [ "$PATHWAY" = "isup" ]; then
    step "Container state ($ANSIBLE_DOCKER_CONTAINER)"

    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+ docker inspect -f %s %s\n' "'{{.State.Running}}'" "$ANSIBLE_DOCKER_CONTAINER"
        printf '+ docker exec %s systemctl is-active %s@%s\n' "$ANSIBLE_DOCKER_CONTAINER" "$ANSIBLE_DOCKER_SERVICE" "$ANSIBLE_DOCKER_INSTANCE"
        exit 0
    fi

    if ! docker inspect -f '{{.State.Running}}' "$ANSIBLE_DOCKER_CONTAINER" >/dev/null 2>&1; then
        die "container '$ANSIBLE_DOCKER_CONTAINER' does not exist"
    fi

    if ! docker inspect -f '{{.State.Running}}' "$ANSIBLE_DOCKER_CONTAINER" 2>/dev/null | grep -qx true; then
        printf '   container: %s — not running\n' "$ANSIBLE_DOCKER_CONTAINER" >&2
        die "container '$ANSIBLE_DOCKER_CONTAINER' is down (start it with: docker start $ANSIBLE_DOCKER_CONTAINER)"
    fi
    printf '   container: %s — running\n' "$ANSIBLE_DOCKER_CONTAINER"

    # Running the container is not the same as the server being up: the unit may
    # have failed or still be starting. systemctl's own vocabulary is reported
    # verbatim so a failed unit is not mistaken for a slow one.
    unit="${ANSIBLE_DOCKER_SERVICE}@${ANSIBLE_DOCKER_INSTANCE}"
    state=$(docker exec "$ANSIBLE_DOCKER_CONTAINER" systemctl is-active "$unit" 2>/dev/null || true)
    if [ "$state" != "active" ]; then
        printf '   unit:      %s — %s\n' "$unit" "${state:-unreachable}" >&2
        die "container '$ANSIBLE_DOCKER_CONTAINER' is running but $unit is '${state:-unreachable}'"
    fi
    printf '   unit:      %s — active\n' "$unit"

    exit 0
fi

if [ "$PATHWAY" = "helgrind" ]; then
    step "Helgrind ($HELGRIND_CONTAINER)"

    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+ docker inspect -f %s %s (must report true)\n' \
            "'{{.State.Running}}'" "$HELGRIND_CONTAINER"
        printf '+ docker exec -i %s sh -c %s > %s 2>&1\n' "$HELGRIND_CONTAINER" \
            "'cd $HELGRIND_WORKDIR; valgrind --tool=helgrind --fair-sched=yes --num-callers=30 --suppressions=$HELGRIND_SUPPRESSIONS --log-fd=2 $HELGRIND_BIN -C $HELGRIND_ROOT -c $HELGRIND_CONFIG -i $HELGRIND_INSTANCE'" \
            "$HELGRIND_HOST_LOG"
    else
        if ! docker inspect -f '{{.State.Running}}' "$HELGRIND_CONTAINER" 2>/dev/null | grep -qx true; then
            die "container '$HELGRIND_CONTAINER' is not running (start it with: docker start $HELGRIND_CONTAINER)"
        fi

        if ! docker exec "$HELGRIND_CONTAINER" test -x "$HELGRIND_BIN" 2>/dev/null; then
            die "helgrind: no executable at '$HELGRIND_BIN' in '$HELGRIND_CONTAINER'"
        fi

        state=$(docker exec "$HELGRIND_CONTAINER" systemctl is-active "ufsrvstun@$HELGRIND_INSTANCE" 2>/dev/null || true)
        case "$state" in
            inactive|failed) ;;
            *) die "helgrind: ufsrvstun@$HELGRIND_INSTANCE is '$state' in '$HELGRIND_CONTAINER' — stop it first:
  docker exec $HELGRIND_CONTAINER systemctl stop ufsrvstun@$HELGRIND_INSTANCE
  this pathway runs only against a stopped instance and will not kill a running service" ;;
        esac

        sanitised=""
        if docker exec "$HELGRIND_CONTAINER" sh -c \
                "ldd '$HELGRIND_BIN' 2>/dev/null | grep -Eq 'lib(asan|ubsan|tsan|msan)'"; then
            sanitised="sanitizer runtime library"
        elif docker exec "$HELGRIND_CONTAINER" sh -c \
                "grep -a -q -E '(__asan_init|__ubsan_handle|__tsan_init|__msan_init)' '$HELGRIND_BIN'"; then
            sanitised="embedded sanitizer symbols"
        elif docker exec "$HELGRIND_CONTAINER" sh -c \
                "ASAN_OPTIONS=help=1 UBSAN_OPTIONS=help=1 '$HELGRIND_BIN' --version 2>&1 | grep -qi sanitizer"; then
            sanitised="sanitizer runtime banner"
        fi
        if [ -n "$sanitised" ]; then
            die "helgrind: '$HELGRIND_BIN' is sanitizer-instrumented ($sanitised) — Valgrind cannot run it; rebuild/deploy with sanitizers off"
        fi

        # The instance is already stopped — checked above — so Helgrind can
        # bind the server ports. Nothing here starts or stops it.
        inner="cd '$HELGRIND_WORKDIR'
valgrind --tool=helgrind --fair-sched=yes --num-callers=30 \
  --suppressions='$HELGRIND_SUPPRESSIONS' \
  --log-fd=2 \
  '$HELGRIND_BIN' -C '$HELGRIND_ROOT' -c '$HELGRIND_CONFIG' -i '$HELGRIND_INSTANCE'"

        printf '+ docker exec -i %s sh -c (helgrind against the stopped instance)\n' "$HELGRIND_CONTAINER"
        status=0
        # Valgrind writes to fd 2; the host redirect captures it. Nothing is
        # written inside the container, so service-user ownership cannot block
        # the log.
        docker exec -i "$HELGRIND_CONTAINER" sh -c "$inner" > "$HELGRIND_HOST_LOG" 2>&1 || status=$?

        printf '   log: %s\n' "$HELGRIND_HOST_LOG"
        [ "$status" -ne 0 ] && printf '   note: helgrind exited %s (the guest is stopped by hand, so this is expected)\n' "$status"
        helgrind_verdict "$HELGRIND_HOST_LOG"
        exit $?
    fi
fi

if [ "$PATHWAY" = "remote" ]; then
    step "Remote deploy ($ANSIBLE_REMOTE_LIMIT -> $ANSIBLE_REMOTE_PLAYBOOK)"
    run_deploy "$ANSIBLE_PROD_INVENTORY" "$ANSIBLE_REMOTE_LIMIT" "$ANSIBLE_REMOTE_PLAYBOOK"
fi

# ── Pathway: mirror ─────────────────────────────────────────────────────────

if [ "$PATHWAY" = "mirror" ]; then
    step "Mirror to GitHub"
    T2GM_BIN="${T2GM:-$HOME/repos/tig_github_mirror/t2gm.sh}"
    VARS_FILE="${REPO_VARS:-$PWD/devops/ansible/vars/$(basename "$PWD").yml}"

    if [ ! -x "$T2GM_BIN" ]; then
        die "t2gm.sh not found at $T2GM_BIN (set T2GM=/path/to/t2gm.sh)"
    fi
    if [ ! -f "$VARS_FILE" ]; then
        die "mirror vars file not found at $VARS_FILE"
    fi

    margs=("$T2GM_BIN" "$VARS_FILE")
    for o in ${MIRROR_OPTS//,/ }; do
        [ -n "$o" ] && margs+=("--$o")
    done
    run "${margs[@]}"
fi

printf '\n==> done\n'
