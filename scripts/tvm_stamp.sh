# Sourced by the scripts/build_*.sh third-party builds.
#
# Each of those builds configures once ("if [ ! -f Makefile ]") and then only
# runs make, so a change to what the build is made from -- the script itself
# (configure flags, cache variables), a TVM config or shim file, the
# submodule's revision, or a static library it links -- never reached an
# existing build tree: the old configure results and objects were reused and
# the stale program shipped until someone deleted output/.
#
# tvm_stamp_begin BUILDDIR INPUT...
#   Hashes the inputs: files by content (a missing file counts too), git
#   checkouts by HEAD plus whether they have local modifications, and CC /
#   TVM_IOS_CROSS from the environment. If the hash differs from the one
#   recorded by the last successful build, the build directory is emptied so
#   the build starts from scratch. The new hash is recorded only when the
#   script exits with status 0, so a failed build is retried from scratch too.

tvm_stamp_hash() {
    for _in in "$@"; do
        if [ -d "$_in" ] && git -C "$_in" rev-parse --git-dir >/dev/null 2>&1; then
            printf 'git %s %s ' "$_in" "$(git -C "$_in" rev-parse HEAD 2>/dev/null)"
            if git -C "$_in" diff --quiet HEAD -- 2>/dev/null; then echo clean; else echo dirty; fi
        elif [ -f "$_in" ]; then
            printf 'file %s ' "$_in"; shasum < "$_in"
        else
            echo "missing $_in"
        fi
    done
    echo "CC=${CC:-} TVM_IOS_CROSS=${TVM_IOS_CROSS:-}"
}

tvm_stamp_begin() {
    _stamp_dir="$1"; shift
    mkdir -p "$_stamp_dir"
    _stamp_file="$_stamp_dir/.tvm-inputs"
    _stamp_sig="$(tvm_stamp_hash "$@" | shasum | cut -d' ' -f1)"
    if [ ! -f "$_stamp_file" ] || [ "$(cat "$_stamp_file")" != "$_stamp_sig" ]; then
        echo "tvm_stamp: inputs of $_stamp_dir changed, rebuilding from scratch" >&2
        find "$_stamp_dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    fi
    trap 'tvm_stamp_status=$?; if [ "$tvm_stamp_status" -eq 0 ]; then echo "$_stamp_sig" > "$_stamp_file"; fi; exit $tvm_stamp_status' EXIT
}
