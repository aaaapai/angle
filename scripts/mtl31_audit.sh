#!/usr/bin/env bash
#
# Copyright 2026 The ANGLE Project Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Static audit for the Metal ES 3.1 work (verification plan L0).  These checks are intentionally
# grep-based: they catch policy regressions such as a Metal 3 API slipping in, a CPU readback
# path added to an ES 3.1 feature, or a feature marker that has no registry entry.
#
# Usage: scripts/mtl31_audit.sh [--registry <path to 07-索引与登记表.md>]
#
# The registry lives outside the ANGLE checkout.  Set MTL31_REGISTRY or pass --registry to point
# at it; check 6 and check 8 are skipped (with a warning) when it is not available.

set -u

failures=0
warnings=0

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "${repo_root}"

registry=${MTL31_REGISTRY:-}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --registry)
            registry=$2
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

metal_dir=src/libANGLE/renderer/metal
msl_dir=src/compiler/translator/msl

# Files that carry the ES 3.1 implementation.  The CPU-readback check is scoped to these files so
# that pre-existing, unrelated diagnostics such as SurfaceMtl's glReadPixels comment do not trip it.
es31_files=(
    "${metal_dir}/ContextMtl.mm"
    "${metal_dir}/DisplayMtl.mm"
    "${metal_dir}/ProgramExecutableMtl.mm"
    "${metal_dir}/ProgramMtl.mm"
    "${metal_dir}/mtl_command_buffer.mm"
    "${metal_dir}/mtl_pipeline_cache.mm"
    "${metal_dir}/mtl_resources.mm"
    "${metal_dir}/BufferMtl.mm"
    "${metal_dir}/TextureMtl.mm"
    "${msl_dir}/TranslatorMSL.cpp"
    "${msl_dir}/EmitMetal.cpp"
    "${msl_dir}/ProgramPrelude.cpp"
    "${msl_dir}/Pipeline.cpp"
    "${msl_dir}/RewritePipelines.cpp"
    "${msl_dir}/Layout.cpp"
)

pass() { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; failures=$((failures + 1)); }
warn() { printf 'WARN  %s\n' "$1"; warnings=$((warnings + 1)); }

check_no_matches() {
    local description=$1
    shift
    local matches
    matches=$("$@" 2>/dev/null || true)
    if [[ -z "${matches}" ]]; then
        pass "${description}"
    else
        fail "${description}"
        printf '%s\n' "${matches}" | sed 's/^/         /'
    fi
}

check_no_matches "1. no Metal 3-only API symbols" \
    grep -rnE -e 'MTLGPUFamilyApple9' -e 'MTLGPUFamilyApple10' \
        -e 'MTLResidencySet' -e 'MTLAccelerationStructure' \
        -e 'MTLDynamicLibrary' -e 'MTLBinaryArchive' "${metal_dir}"

check_no_matches "2. no mesh/object shader symbols" \
    grep -rnE -e 'meshFunction' -e 'objectFunction' \
        -e '\[\[object_data\]\]' -e '\[\[mesh_grid_properties\]\]' \
        "${metal_dir}" "${msl_dir}"

check_no_matches "3. no Metal 3 availability branches" \
    grep -rnE -e '@available\(iOS 16' -e '@available\(macOS 13' \
        "${metal_dir}" "${msl_dir}"

check_no_matches "4. no CPU readback / emulation in ES 3.1 paths" \
    grep -nE -e 'glReadPixels' -e 'glGetBufferSubData' -e 'std::thread' \
        -e 'glFinish' "${es31_files[@]}"

# Check 5 is a review aid rather than a hard failure: endEncoding is a normal command-buffer
# transition, so the script can only flag places that need a human to confirm the call is not
# being used as a fragment-barrier or separable-pipeline substitute.
if grep -qE 'endEncoding.*(barrier|Barrier)' "${metal_dir}" "${msl_dir}" 2>/dev/null; then
    fail "5. endEncoding is not used as a barrier workaround"
    grep -nE 'endEncoding.*(barrier|Barrier)' "${metal_dir}" "${msl_dir}" | sed 's/^/         /'
else
    pass "5. no endEncoding barrier/separable workaround pattern"
fi

# Check 6: the registry may only contain the approved EX-1 emulation row.
if [[ -n "${registry}" && -f "${registry}" ]]; then
    # Only the implementation-method column counts; the word also appears in prose that explains
    # why an item is *not* emulated.
    emulated_rows=$(awk -F'|' '/^\| *`MTL31-/ { method=$4; gsub(/^ +| +$/, "", method);
        if (method ~ /^\*?\*?模拟/ && method !~ /EX-1/) print NR": "$0 }' "${registry}")
    if [[ -z "${emulated_rows}" ]]; then
        pass "6. registry has no unapproved emulation rows"
    else
        fail "6. registry contains emulation rows other than EX-1"
        printf '%s\n' "${emulated_rows}" | sed 's/^/         /'
    fi
else
    warn "6. registry not provided (set MTL31_REGISTRY); skipped"
fi

# Check 7: every TODO/FIXME in a file that already carries MTL31 markers must be traceable.
untracked_todos=$(
    grep -rl 'MTL31-' "${metal_dir}" "${msl_dir}" 2>/dev/null |
        xargs -r grep -nE '(TODO|FIXME)' 2>/dev/null |
        grep -vE 'kpiddington|None of the feature conditions' |
        grep -vE 'TODO\(|FIXME\(|MTL31-[A-Za-z0-9-]+' || true
)
if [[ -z "${untracked_todos}" ]]; then
    pass "7. no untracked TODO/FIXME in MTL31 files"
else
    fail "7. untracked TODO/FIXME in MTL31 files"
    printf '%s\n' "${untracked_todos}" | sed 's/^/         /'
fi

# Check 8: every MTL31 marker in the source must exist in the registry.
if [[ -n "${registry}" && -f "${registry}" ]]; then
    missing_ids=""
    while read -r id; do
        id=${id%-}
        [[ -z "${id}" ]] && continue
        if ! grep -q "\`${id}\`" "${registry}"; then
            missing_ids+="${id}"$'\n'
        fi
    done < <(grep -rhoE 'MTL31-[A-Z][A-Z0-9]*-[A-Za-z0-9][A-Za-z0-9-]*' "${metal_dir}" \
                 "${msl_dir}" 2>/dev/null | sort -u)
    if [[ -z "${missing_ids}" ]]; then
        pass "8. every source MTL31 marker has a registry entry"
    else
        fail "8. source markers missing from the registry"
        printf '%s' "${missing_ids}" | sed 's/^/         /'
    fi
else
    warn "8. registry not provided (set MTL31_REGISTRY); skipped"
fi

# Check 9: defense-in-depth pointers.  Raw Metal handles must stay inside the wrappers, results
# must not be discarded, and ES 3.1 offset math must use CheckedNumeric.  Only a subset can be
# checked mechanically; the script flags the rest for review.
raw_handles=$(
    grep -nE '\bid<MTL(Buffer|Texture)>' "${es31_files[@]}" 2>/dev/null |
        grep -vE 'mtl_resources\.(h|mm)|mtl_command_buffer\.(h|mm)|\.get\(\)' || true
)
if [[ -z "${raw_handles}" ]]; then
    pass "9a. no raw id<MTLBuffer>/id<MTLTexture> in ES 3.1 business paths"
else
    fail "9a. raw Metal handles cross the wrapper boundary"
    printf '%s\n' "${raw_handles}" | sed 's/^/         /'
fi
ignored_results=$(grep -nE '^\s*\(void\)[A-Za-z_].*\(' "${es31_files[@]}" 2>/dev/null || true)
if [[ -z "${ignored_results}" ]]; then
    pass "9b. no explicitly discarded angle::Result values"
else
    warn "9b. review explicitly discarded return values"
    printf '%s\n' "${ignored_results}" | sed 's/^/         /'
fi

# Check 10: the 0x3004 regression guard.  conformant must be min(supported, 3.0); a hard-coded
# 3.2 makes GetClientVersion() raise every backwards-compatible request above supported and
# Context::initialize() rejects it with EGL_BAD_ATTRIBUTE.
conformant_fn=$(sed -n '/^gl::Version DisplayMtl::getMaxConformantESVersion/,/^}/p' \
                    "${metal_dir}/DisplayMtl.mm")
if printf '%s' "${conformant_fn}" | grep -q 'std::min(getMaxSupportedESVersion' &&
    ! printf '%s' "${conformant_fn}" | grep -q 'gl::Version(3, 2)'; then
    pass "10. conformant <= supported (0x3004 regression guard)"
else
    fail "10. getMaxConformantESVersion violates conformant <= supported"
    printf '%s\n' "${conformant_fn}" | sed 's/^/         /'
fi

printf '\n%d failure(s), %d warning(s)\n' "${failures}" "${warnings}"
[[ ${failures} -eq 0 ]]
