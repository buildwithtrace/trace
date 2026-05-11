#!/bin/bash

# Script to convert PNG assets to multiple formats and sizes
# Uses macOS sips and other built-in tools

# Error handling is done explicitly with conditionals

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Standard sizes for icons/logos
STANDARD_SIZES=(16 32 48 64 128 256 512 1024)

# Find PNG files - if a specific file is provided as argument, use that; otherwise find all
if [ $# -gt 0 ]; then
    PNG_FILES=("$@")
else
    PNG_FILES=($(find . -maxdepth 1 -name "*.png" -type f))
fi

if [ ${#PNG_FILES[@]} -eq 0 ]; then
    echo "No PNG files found"
    exit 1
fi

echo "Found ${#PNG_FILES[@]} PNG file(s) to process"

# Function to create SVG wrapper (embeds PNG as base64)
create_svg() {
    local input_file="$1"
    local output_file="$2"
    local width="$3"
    local height="$4"
    
    # Get base64 encoded image (use stdin for macOS base64 compatibility)
    local base64_data=$(cat "$input_file" | base64 | tr -d '\n')
    
    # Create SVG with embedded PNG
    cat > "$output_file" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<svg width="${width}" height="${height}" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
  <image width="${width}" height="${height}" xlink:href="data:image/png;base64,${base64_data}"/>
</svg>
EOF
}

# Function to create ICNS from PNG
create_icns() {
    local input_file="$1"
    local size="$2"
    local output_file="$3"
    
    # Create temporary iconset directory
    local tmp_dir=$(mktemp -d)
    local iconset_name=$(basename "$input_file" .png).iconset
    local iconset_path="$tmp_dir/$iconset_name"
    mkdir -p "$iconset_path"
    
    # Create all required sizes for ICNS
    sips -z 16 16 "$input_file" --out "$iconset_path/icon_16x16.png" > /dev/null 2>&1 || return 1
    sips -z 32 32 "$input_file" --out "$iconset_path/icon_16x16@2x.png" > /dev/null 2>&1 || return 1
    sips -z 32 32 "$input_file" --out "$iconset_path/icon_32x32.png" > /dev/null 2>&1 || return 1
    sips -z 64 64 "$input_file" --out "$iconset_path/icon_32x32@2x.png" > /dev/null 2>&1 || return 1
    sips -z 128 128 "$input_file" --out "$iconset_path/icon_128x128.png" > /dev/null 2>&1 || return 1
    sips -z 256 256 "$input_file" --out "$iconset_path/icon_128x128@2x.png" > /dev/null 2>&1 || return 1
    sips -z 256 256 "$input_file" --out "$iconset_path/icon_256x256.png" > /dev/null 2>&1 || return 1
    sips -z 512 512 "$input_file" --out "$iconset_path/icon_256x256@2x.png" > /dev/null 2>&1 || return 1
    sips -z 512 512 "$input_file" --out "$iconset_path/icon_512x512.png" > /dev/null 2>&1 || return 1
    sips -z 1024 1024 "$input_file" --out "$iconset_path/icon_512x512@2x.png" > /dev/null 2>&1 || return 1
    
    # Convert iconset to ICNS
    if iconutil -c icns "$iconset_path" -o "$output_file" 2>/dev/null; then
        # Cleanup and return success
        rm -rf "$tmp_dir"
        return 0
    else
        # Cleanup and return failure
        rm -rf "$tmp_dir"
        return 1
    fi
}

# Function to create XPM from PNG
create_xpm() {
    local input_file="$1"
    local output_file="$2"
    
    # Try ImageMagick first
    if command -v convert &> /dev/null; then
        convert "$input_file" "$output_file" 2>/dev/null && return 0
    fi
    
    # Try Python with PIL/Pillow
    if command -v python3 &> /dev/null; then
        python3 -c "
import sys
try:
    from PIL import Image
    img = Image.open('$input_file')
    img.save('$output_file', 'XPM')
    sys.exit(0)
except:
    sys.exit(1)
" 2>/dev/null && return 0
    fi
    
    # If both fail, skip silently
    return 1
}

# Process each PNG file
for png_file in "${PNG_FILES[@]}"; do
    filename=$(basename "$png_file")
    basename_no_ext="${filename%.png}"
    
    echo "Processing: $filename"
    
    # Get original dimensions
    width=$(sips -g pixelWidth "$png_file" | tail -1 | awk '{print $2}')
    height=$(sips -g pixelHeight "$png_file" | tail -1 | awk '{print $2}')
    
    # Create base format conversions (original size)
    echo "  Creating base format conversions..."
    
    # SVG (original size)
    svg_file="${basename_no_ext}.svg"
    create_svg "$png_file" "$svg_file" "$width" "$height"
    echo "    Created: $svg_file"
    
    # ICNS (original size) - skip if already exists
    icns_file="${basename_no_ext}.icns"
    if [ ! -f "$icns_file" ]; then
        if create_icns "$png_file" "$width" "$icns_file" 2>/dev/null; then
            echo "    Created: $icns_file"
        fi
    else
        echo "    Skipped: $icns_file (already exists)"
    fi
    
    # ICO (original size) - skip if already exists
    ico_file="${basename_no_ext}.ico"
    if [ ! -f "$ico_file" ]; then
        sips -s format ico "$png_file" --out "$ico_file" > /dev/null 2>&1 || true
        if [ -f "$ico_file" ]; then
            echo "    Created: $ico_file"
        fi
    else
        echo "    Skipped: $ico_file (already exists)"
    fi
    
    # XPM (original size)
    xpm_file="${basename_no_ext}.xpm"
    if create_xpm "$png_file" "$xpm_file"; then
        echo "    Created: $xpm_file"
    fi
    
    # Create different sizes and organize by size
    echo "  Creating size variants..."
    for size in "${STANDARD_SIZES[@]}"; do
        size_dir="${size}x${size}"
        mkdir -p "$size_dir"
        
        # Resize PNG
        resized_png="${size_dir}/${basename_no_ext}_${size}x${size}.png"
        sips -z "$size" "$size" "$png_file" --out "$resized_png" > /dev/null 2>&1
        
        # Create format conversions for this size
        # SVG
        resized_svg="${size_dir}/${basename_no_ext}_${size}x${size}.svg"
        create_svg "$resized_png" "$resized_svg" "$size" "$size"
        
        # ICNS
        resized_icns="${size_dir}/${basename_no_ext}_${size}x${size}.icns"
        create_icns "$resized_png" "$size" "$resized_icns" 2>/dev/null || true
        
        # ICO
        resized_ico="${size_dir}/${basename_no_ext}_${size}x${size}.ico"
        sips -s format ico "$resized_png" --out "$resized_ico" > /dev/null 2>&1 || true
        
        # XPM
        resized_xpm="${size_dir}/${basename_no_ext}_${size}x${size}.xpm"
        create_xpm "$resized_png" "$resized_xpm" || true
        
        echo "    Created size variant: $size_dir/"
    done
    
    echo "  Completed: $filename"
    echo ""
done

echo "All conversions complete!"
