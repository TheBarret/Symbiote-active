#ifndef ASSETS_FONTS_H
#define ASSETS_FONTS_H
#include <stdint.h>

/* Standard Default & Western Latin (`default`, `lat`, `iso`)

    These are your standard European terminal fonts, mostly variations of ISO 8859 character sets
    (Western, Central, and Northern European languages):

    - `default8x16.psf`, `default8x9.psf`, `def2_8x16.psf`:
       Standard VGA/Linux console fonts. safe, universal choices.
    - `lat0-...` through `lat9-...`:
       Standard Latin script fonts optimized for different Euro-code standards
       (e.g., `lat0` includes the Euro symbol via ISO 8859-15; `lat1` is classic ISO 8859-1 Latin-1).
    - `iso01` to `iso14`: Direct implementations of ISO/IEC 8859 regional standards:
    - `iso01`: Western Europe (Latin-1)
    - `iso02`: Central/Eastern Europe (Latin-2)
    - `iso03` / `iso04`: South/North European
    - `iso10` / `iso14`: Nordic and Celtic variants

    ### IBM PC / DOS Code Pages (`cp...`, `alt`)

    These replicate vintage MS-DOS hardware character sets,
    including the full legacy IBM box-drawing characters, and block elements:

    - `cp850-...`: Code Page 850 (Multilingual Latin-1 / Western DOS).
    - `cp857-...`: Code Page 857 (Turkish DOS).
    - `cp865-...`: Code Page 865 (Nordic DOS).
    - `cp866-...`: Code Page 866 (Cyrillic DOS).
    - `alt-...`, `altb-...`, `altc-...`: Alternative IBM VGA hardware font sets.

    ### Cyrillic & Eastern European (`Cyr`, `ruscii`, `koi8`, `Mik`)

    Fonts built specifically for Russian, Ukrainian, Bulgarian, and other Cyrillic-script environments:

    - `Cyr_a...`: Standard Cyrillic console fonts in various heights ($8\times8$, $8\times14$, $8\times16$).
    - `ruscii_...`: "Russian ASCII" — maps Cyrillic characters onto the upper half of standard IBM code pages.
    - `koi8-...`, `koi8u_...`, `koi8r_...`: KOI8 encoding standard fonts (commonly used in early Unix/Linux systems in Russia and Ukraine).
    - `Mik_...`: Bulgarian MIK encoding font standard.

    ### Greek (`gr`, `greek`, `gr737`)
    - `gr...`, `greek.psf`, `grfixed.psf`: General Greek language console fonts.
    - `gr737-...`: Code Page 737 (Hellenic/Greek DOS hardware font).

    ### Multi-Script / Universal (`LatArCyrHeb`)
    - `LatArCyrHeb-...`: large font bundle covering:
        * Latin
        * Arabic
        * Cyrillic
        * and Hebrew simultaneously.

    ### Regional & Asian Scripts (`Goha`, `arm`, `tis`, `tcvn`, `viscii`)
    - `Goha-...`, `GohaClassic-...`: Georgian script fonts.
    - `arm8.psf`: Armenian script font.
    - `tis-...`: Thai Industrial Standard (TIS) fonts for Thai language support.
    - `tcvn...`, `viscii...`: Vietnamese standard character encoding fonts.

    ### Accessibility & Special Purpose (`brl`)
    - `brl-08.psf` through `brl-16.psf`: Braille pattern fonts.
      These replace standard text glyphs with Unicode Braille dot configurations,
      sometimes used for rendering primitive high-resolution terminal graphics or accessibility features.
 */


#include "Cyr_a8x14.h"
#include "Cyr_a8x16.h"
#include "Cyr_a8x8.h"
#include "Goha_12.h"
#include "Goha_14.h"
#include "Goha_16.h"
#include "GohaClassic_12.h"
#include "GohaClassic_14.h"
#include "GohaClassic_16.h"
#include "LatArCyrHeb_08.h"
#include "LatArCyrHeb_14.h"
#include "LatArCyrHeb_16_.h"
#include "LatArCyrHeb_16.h"
#include "LatArCyrHeb_19.h"
#include "Mik_8x16.h"
#include "alt_8x14.h"
#include "alt_8x16.h"
#include "alt_8x8.h"
#include "altb_8x16.h"
#include "altc_8x16.h"
#include "aply16.h"
#include "arm8.h"
#include "brl_08.h"
#include "brl_10.h"
#include "brl_12.h"
#include "brl_14.h"
#include "brl_16.h"
#include "cp850_8x14.h"
#include "cp850_8x16.h"
#include "cp850_8x8.h"
#include "cp857_8x14.h"
#include "cp857_8x16.h"
#include "cp857_8x8.h"
#include "cp865_8x14.h"
#include "cp865_8x16.h"
#include "cp865_8x8.h"
#include "cp866_8x16.h"
#include "def2_8x16.h"
#include "default8x16.h"
#include "default8x9.h"
#include "gr_f14.h"
#include "gr_f16.h"
#include "gr737_8x8_2.h"
#include "gr737_8x8.h"
#include "gr737_9x14_2.h"
#include "gr737_9x14.h"
#include "gr737_9x16_2.h"
#include "gr737_9x16_medieval.h"
#include "gr737_9x16.h"
#include "gr8x14.h"
#include "gr8x16.h"
#include "gr8x6.h"
#include "gr8x7.h"
#include "gr8x8.h"
#include "grcourier.h"
#include "greek.h"
#include "grfixed.h"
#include "iso01_f08.h"
#include "iso01_f14.h"
#include "iso01_f16.h"
#include "iso01a_8x14.h"
#include "iso01a_8x16.h"
#include "iso01a_8x8.h"
#include "iso02_f08.h"
#include "iso02_f14.h"
#include "iso02_f16.h"
#include "iso02g.h"
#include "iso03_f08.h"
#include "iso03_f14.h"
#include "iso03_f16.h"
#include "iso03g.h"
#include "iso04_f08.h"
#include "iso04_f14.h"
#include "iso04_f16.h"
#include "iso05_f08.h"
#include "iso05_f14.h"
#include "iso05_f16.h"
#include "iso06_f08.h"
#include "iso06_f14.h"
#include "iso06_f16.h"
#include "iso07_f08.h"
#include "iso07_f14.h"
#include "iso07_f16.h"
#include "iso08_f08.h"
#include "iso08_f14.h"
#include "iso08_f16.h"
#include "iso09_f08.h"
#include "iso09_f14.h"
#include "iso09_f16.h"
#include "iso10_f08.h"
#include "iso10_f14.h"
#include "iso10_f16.h"
#include "iso14_f16.h"
#include "ka8x16thin_1.h"
#include "koi8_14.h"
#include "koi8_8x14.h"
#include "koi8_8x16.h"
#include "koi8_8x8.h"
#include "koi8b_8x16.h"
#include "koi8c_8x16.h"
#include "koi8r_8x8.h"
#include "koi8u_8x14.h"
#include "koi8u_8x16.h"
#include "koi8u_8x8.h"
#include "lat0_08.h"
#include "lat0_10.h"
#include "lat0_12.h"
#include "lat0_14.h"
#include "lat0_16.h"
#include "lat0_sun16.h"
#include "lat1_08.h"
#include "lat1_10.h"
#include "lat1_12.h"
#include "lat1_14.h"
#include "lat1_16.h"
#include "lat1u_08.h"
#include "lat1u_10.h"
#include "lat1u_12.h"
#include "lat1u_14.h"
#include "lat1u_16.h"
#include "lat2_08.h"
#include "lat2_10.h"
#include "lat2_12.h"
#include "lat2_14.h"
#include "lat2_16.h"
#include "lat2_sun16.h"
#include "lat2u_08.h"
#include "lat2u_10.h"
#include "lat2u_12.h"
#include "lat2u_14.h"
#include "lat2u_16.h"
#include "lat4_08.h"
#include "lat4_10.h"
#include "lat4_12.h"
#include "lat4_14.h"
#include "lat4_16_.h"
#include "lat4_16.h"
#include "lat4_19.h"
#include "lat4a_08.h"
#include "lat4a_10.h"
#include "lat4a_12.h"
#include "lat4a_14.h"
#include "lat4a_16_.h"
#include "lat4a_16.h"
#include "lat4a_19.h"
#include "lat4u_08.h"
#include "lat4u_10.h"
#include "lat4u_12.h"
#include "lat4u_14.h"
#include "lat4u_16_.h"
#include "lat4u_16.h"
#include "lat4u_19.h"
#include "lat5u_16.h"
#include "lat7_14.h"
#include "lat9_08.h"
#include "lat9_10.h"
#include "lat9_12.h"
#include "lat9_14.h"
#include "lat9_16.h"
#include "lat9u_08.h"
#include "lat9u_10.h"
#include "lat9u_12.h"
#include "lat9u_14.h"
#include "lat9u_16.h"
#include "lat9v_08.h"
#include "lat9v_10.h"
#include "lat9v_12.h"
#include "lat9v_14.h"
#include "lat9v_16.h"
#include "lat9w_08.h"
#include "lat9w_10.h"
#include "lat9w_12.h"
#include "lat9w_14.h"
#include "lat9w_16.h"
#include "lat9wbrl_08.h"
#include "lat9wbrl_10.h"
#include "lat9wbrl_12.h"
#include "lat9wbrl_14.h"
#include "lat9wbrl_16.h"
#include "latarcyrheb_sun16.h"
#include "ruscii_8x14.h"
#include "ruscii_8x16.h"
#include "ruscii_8x8.h"
#include "t850b.h"
#include "tcvn8x16.h"
#include "tis_phaisarn_f16.h"
#include "tis_ptconsl_f16.h"
#include "tis_ptcufont_f20.h"
#include "tis_ptkmfont_f24.h"
#include "tis_ptlight_f16.h"
#include "tis_ptramafo_f20.h"
#include "tis_ptsmall_f16.h"
#include "viscii10_8x16.h"

#endif /* ASSETS_FONTS_H */
