# mpc-addin-installer: shared by install.sh and uninstall.sh (identical in every addin; only addin.manifest differs).
# MPC's systemd service, its LD_PRELOAD list, and the manifest. ADDIN_LIB_VERSION identifies this copy.
ADDIN_LIB_VERSION=1
# Tests set ADDIN_INSTALL_TEST=1, SYSTEMD_ROOT (a scratch tree holding the unit files) and ADDIN_TEST_LOG.

UNIT_DIRS="/etc/systemd/system /run/systemd/system /usr/lib/systemd/system /lib/systemd/system"
DROPIN_NAME=50-mpc-addins.conf   # one drop-in shared by every addin, when the service sets no LD_PRELOAD itself

svc() {   # systemctl, or a log line under test
    if [ -n "$ADDIN_INSTALL_TEST" ]; then echo "systemctl $*" >> "${ADDIN_TEST_LOG:-/dev/null}"; return 0; fi
    systemctl "$@"
}

# MPC's service: acvs on stock firmware, inmusic-mpc on some modified ones.
mpc_service() {
    for s in acvs inmusic-mpc; do
        for d in $UNIT_DIRS; do
            [ -f "$SYSTEMD_ROOT$d/$s.service" ] && { echo "$s"; return; }
        done
    done
    echo acvs
}

# The unit file or drop-in whose Environment= line sets LD_PRELOAD and wins (the last one systemd reads), if any.
unit_with_preload() {
    found=""
    for d in $UNIT_DIRS; do   # the main unit: the first directory that has it
        f="$SYSTEMD_ROOT$d/$1.service"
        if [ -f "$f" ]; then grep -q '^Environment=.*LD_PRELOAD=' "$f" && found="$f"; break; fi
    done
    for f in $(for d in $UNIT_DIRS; do ls "$SYSTEMD_ROOT$d/$1.service.d/"*.conf 2>/dev/null; done | awk -F/ '{print $NF "\t" $0}' | sort | cut -f2); do
        grep -q '^Environment=.*LD_PRELOAD=' "$f" && found="$f"   # drop-ins apply in name order
    done
    echo "$found"
}

# Rewrite the LD_PRELOAD value on each matching line of a file with an awk program ("add" or "remove" $so).
edit_preload() {   # file mode so
    awk -v mode="$2" -v so="$3" '
    /^Environment=/ && (i = index($0, "LD_PRELOAD=")) {
        pre = substr($0, 1, i - 1); rest = substr($0, i + 11)
        quoted = substr(pre, length(pre), 1) == "\""   # Environment="LD_PRELOAD=/a.so /b.so" runs to the quote
        e = index(rest, quoted ? "\"" : " "); if (!e) e = length(rest) + 1
        val = substr(rest, 1, e - 1); post = substr(rest, e)
        n = split(val, parts, /[: ]+/); out = ""; seen = 0
        for (k = 1; k <= n; k++) {
            if (parts[k] == "") continue
            if (parts[k] == so) { seen = 1; if (mode == "remove") continue }
            out = out (out == "" ? "" : ":") parts[k]
        }
        if (mode == "add" && !seen) out = out (out == "" ? "" : ":") so
        if (out == "") {                              # nothing left: drop the assignment (and its quotes)
            if (quoted) { pre = substr(pre, 1, length(pre) - 1); post = substr(post, 2) }
            sub(/^ +/, "", post); print pre post; next
        }
        print pre "LD_PRELOAD=" out post; next
    }
    { print }' "$1" > "$1.new"
    sed -i 's/^Environment= *$//' "$1.new"
    mv "$1.new" "$1"
}

preload_add() {   # service unit so
    if [ -n "$2" ]; then
        bak="$2.bak-mpc-addins"   # the first edit of a file not ours keeps a backup; the shared drop-in needs none
        [ "$(basename "$2")" = "$DROPIN_NAME" ] || [ -f "$bak" ] || cp "$2" "$bak"
        cp "$2" "$2.prev"
        edit_preload "$2" add "$3"
        grep -qF "$3" "$2" || { mv "$2.prev" "$2"; echo "error: editing $2 failed; restored" >&2; exit 1; }
        rm -f "$2.prev"
    else
        d="$SYSTEMD_ROOT/etc/systemd/system/$1.service.d"
        mkdir -p "$d"
        printf '[Service]\nEnvironment=LD_PRELOAD=%s\n' "$3" > "$d/$DROPIN_NAME.new"
        mv "$d/$DROPIN_NAME.new" "$d/$DROPIN_NAME"
    fi
}

preload_remove() {   # service so: take so out of every LD_PRELOAD; the shared drop-in goes once it preloads nothing
    for d in $UNIT_DIRS; do
        for f in "$SYSTEMD_ROOT$d/$1.service" "$SYSTEMD_ROOT$d/$1.service.d/"*.conf; do
            [ -f "$f" ] && grep -qF "$2" "$f" || continue
            edit_preload "$f" remove "$2"
            if [ "$(basename "$f")" = "$DROPIN_NAME" ] && ! grep -q 'LD_PRELOAD=' "$f"; then rm -f "$f"; fi
        done
    done
}

# addin.manifest, next to install.sh: shell assignments, checked before anything uses them.
#   ADDIN_ID      folder and identity: /data/mpc-addins/<id> (letters, digits, - and _)
#   ADDIN_NAME    shown to the user
#   ADDIN_SO      the library, preloaded into MPC
#   ADDIN_CONF    settings file: installed only when the folder has none, so the user's edits survive ("" none)
#   ADDIN_FILES   other files, replaced on every install ("" none)
#   ADDIN_DONE    a line printed after installing ("" none)
load_manifest() {
    [ -f addin.manifest ] || die "addin.manifest is missing next to install.sh"
    ADDIN_ID=""; ADDIN_NAME=""; ADDIN_SO=""; ADDIN_CONF=""; ADDIN_FILES=""; ADDIN_DONE=""
    . ./addin.manifest
    case "$ADDIN_ID" in ""|*[!A-Za-z0-9_-]*) die "addin.manifest: bad ADDIN_ID '$ADDIN_ID'" ;; esac
    for f in "$ADDIN_SO" $ADDIN_CONF $ADDIN_FILES; do
        case "$f" in ""|*/*|.*|*[!A-Za-z0-9._-]*) die "addin.manifest: bad file name '$f'" ;; esac
    done
    case "$ADDIN_SO" in *.so) ;; *) die "addin.manifest: ADDIN_SO must be a .so" ;; esac
    [ -n "$ADDIN_NAME" ] || ADDIN_NAME="$ADDIN_ID"
    DIR="${DIR:-/data/mpc-addins/$ADDIN_ID}"
}

check_dir() {
    case "$DIR" in /*) ;; *) die "-t must be an absolute path" ;; esac
    case "$DIR" in *[!A-Za-z0-9/._-]*) die "the folder may only contain letters, digits and / . _ -" ;; esac
}
