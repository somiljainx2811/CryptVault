#!/usr/bin/env bash
# End-to-end terminal test: drives the real CLI binary in a sandboxed
# $HOME. Usage: tests/cli_smoke.sh /path/to/cryptvault-cli
set -u
CV="${1:?usage: cli_smoke.sh /path/to/cryptvault-cli}"
SB="$(mktemp -d)"; trap 'rm -rf "$SB"' EXIT
export HOME="$SB/home" XDG_DATA_HOME="$SB/home/.local/share" XDG_CONFIG_HOME="$SB/home/.config" NO_COLOR=1
mkdir -p "$HOME"
pass=0; fail=0
ok()   { pass=$((pass+1)); }
bad()  { fail=$((fail+1)); echo "  [FAIL] $1"; }
check(){ if eval "$2"; then ok; else bad "$1"; fi; }
PW="$SB/pw"; printf 'correct horse\n' > "$PW"; WRONG="$SB/wrong"; printf 'nope\n' > "$WRONG"

echo "=== files side ==="
mkdir -p "$SB/work/sub" && echo hi > "$SB/work/a.txt" && echo x > "$SB/work/.dot"
check "ls lists names"        '"$CV" ls "$SB/work" | grep -qx "a.txt"'
check "ls marks dirs"         '"$CV" ls "$SB/work" | grep -qx "sub/"'
check "ls hides dotfiles"     '! "$CV" ls "$SB/work" | grep -q "^.dot"'
check "ls -a shows dotfiles"  '"$CV" ls -a "$SB/work" | grep -q "^.dot"'
check "ls -la clustered flags" '"$CV" ls -la "$SB/work" | grep -q "rw"'
check "ls missing -> rc 1"    '! "$CV" ls "$SB/nope" 2>/dev/null'
mkdir -p "$SB/work/sub/deeper" && echo q > "$SB/work/sub/deeper/Findme.TXT"
check "find is recursive + case-insensitive" '[ "$("$CV" find findme "$SB/work")" = "$SB/work/sub/deeper/Findme.TXT" ]'
check "find no match -> rc 1"  '! "$CV" find zzzzqq "$SB/work" 2>/dev/null'
check "info shows type"       '"$CV" info "$SB/work/a.txt" | grep -q "Type:      file"'
check "places has Home"       '"$CV" places | grep -q "^Home"'
"$CV" trash put "$SB/work/a.txt"
check "trash put removes it"  '[ ! -e "$SB/work/a.txt" ]'
check "trash list shows it"   '"$CV" trash list | grep -q "a.txt"'
"$CV" trash restore a.txt
check "trash restore returns" '[ -e "$SB/work/a.txt" ]'
"$CV" trash put "$SB/work/a.txt" && "$CV" trash empty
check "trash empty"           '! "$CV" trash list 2>&1 | grep -q "a.txt"'

echo "=== vault side ==="
check "create vault"          '"$CV" vault create secrets --path "$SB/vaults/secrets" --password-file "$PW" | grep -q Created'
check "vault is registered"   '"$CV" vault list | grep -q "^secrets"'
check "ls marks it a vault"   '"$CV" ls -l "$SB/vaults" | grep -q "\[vault\]"'
check "create dup refused"    '! "$CV" vault create secrets --password-file "$PW" 2>/dev/null'
check "wrong password -> rc 3" '"$CV" vault ls secrets --password-file "$WRONG" 2>/dev/null; [ $? -eq 3 ]'
check "empty vault lists"     '[ -z "$("$CV" vault ls secrets --password-file "$PW")" ]'
"$CV" vault mkdir secrets:/docs --password-file "$PW"
printf 'top secret\n' > "$SB/plain.txt"; mkdir -p "$SB/tree/inner"; echo deep > "$SB/tree/inner/d.txt"; echo top > "$SB/tree/t.txt"
check "add file"              '"$CV" vault add secrets:/docs "$SB/plain.txt" --password-file "$PW" | grep -q "1 file"'
check "add folder recursively" '"$CV" vault add secrets:/docs "$SB/tree" --password-file "$PW" | grep -q "2 file"'
check "tree shows nesting"    '"$CV" vault tree secrets --password-file "$PW" | grep -q "d.txt"'
check "get round-trips"       '[ "$("$CV" vault get secrets:/docs/plain.txt --password-file "$PW")" = "top secret" ]'
check "get -o 0600, no clobber" '"$CV" vault get secrets:/docs/plain.txt -o "$SB/out.txt" --password-file "$PW" && [ "$(stat -c %a "$SB/out.txt")" = 600 ] && ! "$CV" vault get secrets:/docs/plain.txt -o "$SB/out.txt" --password-file "$PW" 2>/dev/null'
check "put from stdin"        'echo piped | "$CV" vault put secrets:/docs/p.txt --password-file "$PW" && [ "$("$CV" vault get secrets:/docs/p.txt --password-file "$PW")" = piped ]'
check "mv renames"            '"$CV" vault mv secrets:/docs/p.txt q.txt --password-file "$PW" && "$CV" vault ls secrets:/docs --password-file "$PW" | grep -qx "q.txt"'
check "rm non-empty needs -r" '! "$CV" vault rm secrets:/docs/tree --password-file "$PW" 2>/dev/null && "$CV" vault rm -r secrets:/docs/tree --password-file "$PW"'
check "'..' rejected"         '! "$CV" vault ls "secrets:/docs/.." --password-file "$PW" 2>/dev/null'
check "add refuses overwrite" '! "$CV" vault add secrets:/docs "$SB/plain.txt" --password-file "$PW" 2>/dev/null'
check "--delete-originals"    'cp "$SB/plain.txt" "$SB/gone.txt" && "$CV" vault add secrets "$SB/gone.txt" --delete-originals --password-file "$PW" >/dev/null && [ ! -e "$SB/gone.txt" ]'
check "disk has no plaintext" '! grep -rq "top secret" "$SB/vaults/secrets" && ! grep -rq "plain.txt" "$SB/vaults/secrets"'
check "address by directory"  '"$CV" vault ls "$SB/vaults/secrets" --password-file "$PW" | grep -q docs'
check "password via fd"       '"$CV" vault ls secrets --password-fd 3 3<"$PW" | grep -q docs'
check "password via env"      'CRYPTVAULT_PASSWORD="correct horse" "$CV" vault ls secrets | grep -q docs'
check "forget keeps data"     '"$CV" vault forget secrets >/dev/null && ! "$CV" vault list 2>/dev/null | grep -q secrets && [ -f "$SB/vaults/secrets/vault.meta" ]'
check "register again"        '"$CV" vault register back "$SB/vaults/secrets" && "$CV" vault ls back --password-file "$PW" | grep -q docs'
check "gui request detected"  '"$CV" gui "$SB/work" 2>&1 | grep -q "terminal-only"'
check "bare dir arg -> gui"   '"$CV" "$SB/work" 2>&1 | grep -q "terminal-only"'
check "completion bash"       '"$CV" completion bash | bash -n'
check "completion zsh header" '"$CV" completion zsh | head -1 | grep -q "#compdef cryptvault"'

echo; echo "=== Summary: $pass/$((pass+fail)) checks passed ==="
[ "$fail" -eq 0 ]
