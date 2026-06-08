#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app_dir=${XDG_DATA_HOME:-"$HOME/.local/share"}/applications
icon_root=${XDG_DATA_HOME:-"$HOME/.local/share"}/icons/hicolor
icon_src="$repo_root/cpp/icons/servo.png"
icon_dst="$icon_root/250x250/apps/servoq.png"
desktop_dst="$app_dir/servoq.desktop"
exec_path="$repo_root/target/release/servoq"

if [ ! -f "$icon_src" ]; then
    echo "missing icon: $icon_src" >&2
    exit 1
fi

mkdir -p "$(dirname -- "$icon_dst")" "$app_dir"
install -m 0644 "$icon_src" "$icon_dst"

sed "s|^Exec=.*|Exec=$exec_path %U|" "$repo_root/data/servoq.desktop" > "$desktop_dst"
chmod 0644 "$desktop_dst"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$app_dir" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q "$icon_root" >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi

echo "Installed $desktop_dst"
echo "Installed $icon_dst"
echo "Desktop id: servoq"
echo "Exec: $exec_path %U"
