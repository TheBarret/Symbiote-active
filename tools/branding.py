from PIL import Image

# Basic that converts PNG data into C-lang header.
# version 1.0
# Disclaimer: Made by Gemini

def convert_image_to_header(image_path, output_path, width=None, height=None):
    img = Image.open(image_path).convert("RGBA")
    if width and height:
        img = img.resize((width, height), Image.Resampling.LANCZOS)
    elif width:
        w_percent = (width / float(img.size[0]))
        h_size = int(float(img.size[1]) * float(w_percent))
        img = img.resize((width, h_size), Image.Resampling.LANCZOS)
    elif height:
        h_percent = (height / float(img.size[1]))
        w_size = int(float(img.size[0]) * float(h_percent))
        img = img.resize((w_size, height), Image.Resampling.LANCZOS)

    img_width, img_height = img.size
    pixels = list(img.getdata())

    with open(output_path, "w") as f:
        f.write(f"#ifndef LOGO_H\n#define LOGO_H\n\n")
        f.write(f"#define LOGO_WIDTH {img_width}\n")
        f.write(f"#define LOGO_HEIGHT {img_height}\n\n")
        f.write(f"const unsigned int logo_data[{img_width} * {img_height}] = {{\n    ")
        count = 0
        for r, g, b, a in pixels:
            # Pack into a 32-bit integer (BGRA format commonly used by framebuffers)
            # Adjust to (a << 24) | (r << 16) | (g << 8) | b if your framebuffer expects RGBA
            val = (a << 24) | (r << 16) | (g << 8) | b
            f.write(f"0x{val:08x}, ")
            count += 1
            if count % 8 == 0:
                f.write("\n    ")

        f.write("\n};\n\n#endif // LOGO_H\n")
    print(f"Successfully generated {output_path} ({img_width}x{img_height})")

if __name__ == "__main__":
    convert_image_to_header("logo-small.png", "logo.h", width=200)
