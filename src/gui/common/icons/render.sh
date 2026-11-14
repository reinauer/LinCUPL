#!/bin/sh
# Renders the PNG sizes of the application and toolbar icons from their SVG
# sources.  The PNGs are committed, so building does not need an SVG
# renderer; run this after changing an SVG.  Needs rsvg-convert (librsvg).
set -e
cd "$(dirname "$0")"

mkdir -p png
for name in lincupl linsim; do
    for size in 16 24 32 48 64 128 256 512; do
        rsvg-convert -w "$size" -h "$size" "$name.svg" -o "png/$name-$size.png"
    done
done

# Toolbar and menu icons; 32, 48 and 64 serve high-resolution screens.  The
# application icons are added as app-<name> for the buttons that start the
# other program.
mkdir -p actions/png
for svg in actions/*.svg lincupl.svg linsim.svg; do
    name=$(basename "$svg" .svg)
    case "$svg" in
        actions/*) out="actions/png/$name" ;;
        *) out="actions/png/app-$name" ;;
    esac
    for size in 16 24 32 48 64; do
        rsvg-convert -w "$size" -h "$size" "$svg" -o "$out-$size.png"
    done
done
