#!/tvm/bin/sh
# /tvm/bin/security: macOS's keychain tool as a keychain with no items, for
# programs that keep their secrets there (Claude Code). TVM cannot run the
# host's (an arm64e system binary on macOS; iOS has none), and a failing one
# is not the same as an empty keychain to them: Claude Code 2.1.302 reads
# its login back before saving it, takes any exit status other than "not
# found" (44) or "locked" (36) for a keychain it cannot reach, and then
# refuses to save ("Couldn't save your login. If your Mac's keychain is
# locked, unlock it and log in again."). Lookups and deletions find nothing;
# writes fail plainly, so the program falls back to its own storage (Claude
# Code: ~/.claude/.credentials.json, owner-only).

notfound() {
    echo "security: SecKeychainSearchCopyNext: The specified item could not be found in the keychain." >&2
    exit 44
}
unavailable() {
    echo "security: no keychain is available in TVM" >&2
    exit 1
}

case "$1" in
    find-*-password|find-certificate|find-key|find-identity|delete-*-password)
        notfound ;;
    -i|-q)
        # interactive: commands on stdin, which the caller writes whole
        cat > /dev/null
        unavailable ;;
    *)
        unavailable ;;
esac
