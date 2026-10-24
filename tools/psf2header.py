import os
import glob
import sys

# Basic one-shot tool that converts PSF fonts into C-lang headers.
# version 1.0
# Disclaimer: Made by ClaudeAI

def convert_all_psfs(source_folder, output_folder):
    os.makedirs(output_folder, exist_ok=True)
    psf_files = glob.glob(os.path.join(source_folder, "*.psf"))

    if not psf_files:
        print(f"No .psf files found in '{source_folder}'")
        return

    index_file_path = os.path.join(output_folder, "fonts_manifest.h")
    with open(index_file_path, "w") as index_out:
        index_out.write("/* Auto-generated fonts manifest */\n")
        index_out.write("#ifndef FONTS_MANIFEST_H\n#define FONTS_MANIFEST_H\n\n")
        index_out.write("#include <stdint.h>\n\n")

        for filepath in sorted(psf_files):
            filename = os.path.basename(filepath)
            base_name = os.path.splitext(filename)[0]

            # Sanitize filename into a valid C identifier
            c_name = "".join(c if c.isalnum() else "_" for c in base_name)
            if c_name[0].isdigit():
                c_name = "_" + c_name

            out_header_name = f"{c_name}.h"
            out_header_path = os.path.join(output_folder, out_header_name)

            try:
                with open(filepath, "rb") as f:
                    magic = f.read(4)

                    # Check for PSF1 format (2-byte magic: 0x36, 0x04)
                    if magic[:2] == b'\x36\x04':
                        f.seek(2)
                        mode = f.read(1)[0]
                        charsize = f.read(1)[0]
                        width = 8
                        height = charsize
                        num_glyphs = 512 if (mode & 0x01) else 256
                        bytes_per_glyph = charsize
                        glyph_data = f.read()

                    # Check for PSF2 format (4-byte magic: 0x72, 0xb5, 0x4a, 0x86)
                    elif magic == b'\x72\xb5\x4a\x86':
                        version = int.from_bytes(f.read(4), 'little')
                        headersize = int.from_bytes(f.read(4), 'little')
                        flags = int.from_bytes(f.read(4), 'little')
                        num_glyphs = int.from_bytes(f.read(4), 'little')
                        bytes_per_glyph = int.from_bytes(f.read(4), 'little')
                        height = int.from_bytes(f.read(4), 'little')
                        width = int.from_bytes(f.read(4), 'little')
                        f.seek(headersize)
                        glyph_data = f.read()
                    else:
                        print(f"Skipping {filename}: Unknown font format.")
                        continue

                # Write individual font header file
                with open(out_header_path, "w") as out:
                    out.write(f"/* Font: {filename} | Size: {width}x{height} */\n")
                    guard_name = f"FONT_{c_name.upper()}_H"
                    out.write(f"#ifndef {guard_name}\n#define {guard_name}\n\n")
                    out.write(f"#include <stdint.h>\n\n")
                    out.write(f"#define {c_name.upper()}_WIDTH {width}\n")
                    out.write(f"#define {c_name.upper()}_HEIGHT {height}\n\n")
                    out.write(f"static const uint8_t {c_name}_font[] = {{\n    ")

                    total_bytes = num_glyphs * bytes_per_glyph
                    font_bytes = glyph_data[:total_bytes]

                    for i, b in enumerate(font_bytes):
                        out.write(f"0x{b:02x}, ")
                        if (i + 1) % 12 == 0:
                            out.write("\n    ")

                    out.write(f"\n}};\n\n#endif /* {guard_name} */\n")

                # Register in manifest
                index_out.write(f'#include "{out_header_name}"\n')
                print(f"Converted: {filename:<22} -> {out_header_name} ({width}x{height})")

            except Exception as e:
                print(f"Error processing {filename}: {e}")

        index_out.write("\n#endif /* FONTS_MANIFEST_H */\n")
        print(f"\nAll fonts successfully processed! Manifest generated at: {os.path.join(output_folder, 'fonts_manifest.h')}")

if __name__ == "__main__":
    source_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "./generated_fonts"
    convert_all_psfs(source_dir, output_dir)
