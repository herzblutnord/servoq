#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
app_dir="$data_home/applications"
icon_root="$data_home/icons/hicolor"
icon_src="$repo_root/cpp/icons/servo.png"
desktop_dst="$app_dir/servoq.desktop"
exec_path=${SERVOQ_EXEC_PATH:-"$repo_root/target/release/servoq"}
icon_name=servoq
sizes="16 22 32 48 64 128 256"

if [ ! -f "$icon_src" ]; then
    echo "missing icon: $icon_src" >&2
    exit 1
fi
if [ ! -x "$exec_path" ]; then
    echo "missing executable: $exec_path" >&2
    exit 1
fi

mkdir -p "$app_dir"
rm -f "$icon_root/250x250/apps/$icon_name.png"
rm -f "$icon_root/256x256/apps/$icon_name-source.png"

install_icon_size() {
    size=$1
    dst_dir="$icon_root/${size}x${size}/apps"
    dst="$dst_dir/$icon_name.png"
    mkdir -p "$dst_dir"
    if command -v magick >/dev/null 2>&1; then
        magick "$icon_src" -resize "${size}x${size}" "$dst"
    elif command -v convert >/dev/null 2>&1; then
        convert "$icon_src" -resize "${size}x${size}" "$dst"
    elif command -v xdg-icon-resource >/dev/null 2>&1; then
        tmp=$(mktemp --suffix=.png)
        cp "$icon_src" "$tmp"
        xdg-icon-resource install --mode user --context apps --size "$size" "$tmp" "$icon_name" >/dev/null 2>&1 || cp "$icon_src" "$dst"
        rm -f "$tmp"
    else
        cp "$icon_src" "$dst"
    fi
    chmod 0644 "$dst"
}

for size in $sizes; do
    install_icon_size "$size"
done

sed "s|^Exec=.*|Exec=$exec_path %U|" "$repo_root/data/servoq.desktop" > "$desktop_dst"
chmod 0644 "$desktop_dst"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$app_dir" >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q "$icon_root" >/dev/null 2>&1 || true
fi
if command -v xdg-icon-resource >/dev/null 2>&1; then
    xdg-icon-resource forceupdate --mode user >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi

echo "Installed desktop file: $desktop_dst"
grep -E '^(Name|Exec|Icon|StartupWMClass)=' "$desktop_dst"
echo "Installed icon files:"
find "$icon_root" -path "*/apps/$icon_name.png" -print | sort
