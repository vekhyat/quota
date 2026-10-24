#!/bin/sh
# Installs Quota for the current user, with no compiler and no sudo:
#   curl -fsSL https://raw.githubusercontent.com/vekhyat/quota/main/install.sh | sh
# Remove it again with: sh install.sh --uninstall
set -eu

repo="vekhyat/quota"
prefix="${PREFIX:-$HOME/.local}"
app="$prefix/share/quota"
bin="$prefix/bin/quota"
desktop="$prefix/share/applications/quota.desktop"
icon="$prefix/share/icons/hicolor/scalable/apps/quota.svg"

say() { printf '%s\n' "$*"; }
fail() { printf 'quota: %s\n' "$*" >&2; exit 1; }

if [ "${1:-}" = "--uninstall" ]; then
    rm -rf "$app"
    rm -f "$bin" "$desktop" "$icon"
    say "Removed Quota from $prefix."
    exit 0
fi

[ "$(uname -s)" = "Linux" ] || fail "Quota runs on Linux only."
[ "$(uname -m)" = "x86_64" ] || fail "the prebuilt app is x86_64 only. Build from source instead: https://github.com/$repo#build-from-source"

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -qO "$2" "$1"; }
else
    fail "install curl or wget first."
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM
latest="https://github.com/$repo/releases/latest/download"

say "Downloading Quota..."
fetch "$latest/Quota-x86_64.AppImage" "$work/Quota-x86_64.AppImage" || fail "download failed. Check your connection and try again."
if command -v sha256sum >/dev/null 2>&1 && fetch "$latest/SHA256SUMS" "$work/SHA256SUMS" 2>/dev/null; then
    (cd "$work" && grep ' Quota-x86_64.AppImage$' SHA256SUMS | sha256sum -c --status) \
        || fail "the download does not match its checksum. Try again."
fi

# Unpacking the AppImage means it runs without FUSE, which many distributions lack.
chmod +x "$work/Quota-x86_64.AppImage"
(cd "$work" && ./Quota-x86_64.AppImage --appimage-extract >/dev/null) || fail "could not unpack the app."

rm -rf "$app"
mkdir -p "$(dirname "$app")" "$(dirname "$bin")" "$(dirname "$desktop")" "$(dirname "$icon")"
mv "$work/squashfs-root" "$app"

cat > "$bin" <<EOF
#!/bin/sh
exec "$app/AppRun" "\$@"
EOF
chmod 755 "$bin"

cp "$app/usr/share/icons/hicolor/scalable/apps/quota.svg" "$icon"
cat > "$desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Quota
Comment=Show limits, usage, and cost
Exec="$bin"
Icon=quota
Terminal=false
Categories=Utility;
StartupWMClass=quota
EOF
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$(dirname "$desktop")" 2>/dev/null || true

say "Installed Quota. Open it from your application menu, or run: $bin"
case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *) say "To run it as 'quota', add $prefix/bin to your PATH." ;;
esac
