#ifndef CORE_SERVICES_DECODER_H
#define CORE_SERVICES_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* codebase: 0.2.0 */

/* Frame Decoder
 * Frame    : FRAME_STRUCT
 * Layout   : [ dtype:uint8_t | flags:uint8_t | data : length = dtype-width ]
 */

/* data type tag, first byte of a structured payload */
typedef enum {
    DTYPE_NONE   = 0,   /* undefined / unset (invalid frame)        */
    DTYPE_BYTE   = 1,   /* uint8_t,  1 byte                         */
    DTYPE_INT16  = 2,   /* int16_t,  2 bytes                        */
    DTYPE_INT32  = 3,   /* int32_t,  4 bytes                        */
    DTYPE_UINT16 = 4,   /* uint16_t, 2 bytes                        */
    DTYPE_UINT32 = 5,   /* uint32_t, 4 bytes                        */
    DTYPE_BOOL   = 6,   /* uint8_t,  1 byte, must be 0 or 1         */
    DTYPE_STRING = 7,   /* NUL-terminated, 1..DECODER_STRING_MAX    */
} decoder_dtype;

/* flags, second byte of a structured payload */
#define DECODER_FLAG_BIG_ENDIAN  (1u << 0)  /* data is big-endian on the wire */

/* header size: dtype + flags */
#define DECODER_HEADER_SIZE  2u

/* max STRING payload, not counting the NUL */
#define DECODER_STRING_MAX   255u

/* return codes */
enum decoder_result {
    DECODER_OK                    =  0,  /* a-ok */
    DECODER_ERR_INVALID           = -1,  /* generic error fallback, unexpected */
    DECODER_ERR_ARG               = -2,  /* payload missing or null */
    DECODER_ERR_LENGTH            = -3,  /* length insufficient for type */
    DECODER_ERR_TYPE              = -4,  /* no such type exists */
    DECODER_ERR_FLAG              = -5,  /* no such flag exists */
    DECODER_ERR_WIDTH             = -6,  /* variable-width mismatch */
    DECODER_ERR_BAD_STR           = -7,  /* string data length too large, malformed */
};

/* decoded view: a validated typed value plus its type/flags.
 * `data` points into the original payload (zero copy) and is valid only for the lifetime of the buffer it was decoded from.
 */
typedef struct {
    decoder_dtype dtype;
    uint8_t       flags;
    const uint8_t *data;    /* points into payload + DECODER_HEADER_SIZE */
    uint16_t      len;      /* value length in bytes (NUL included for STRING) */
} decoder_data;

/* Table
 * Fixed-width types have a constant width; STRING is variable.
 * A width of DECODER_WIDTH_VAR marks variable-width types, DTYPE_NONE has width 0 and is always invalid.
 */
#define DECODER_WIDTH_VAR 0xFFFFu

typedef struct {
    uint16_t width;   /* fixed width, or DECODER_WIDTH_VAR */
    bool     valid;   /* DTYPE_NONE and reserved slots are false */
} decoder_tinfo;

/* Exposed for callers that want to validate a dtype without decoding. */
const decoder_tinfo *decoder_typeinfo(decoder_dtype dtype);

/*  Sanity
 * Checks a raw structured payload against every known invariant without interpreting it:
 * non-NULL, length >= header, dtype known, dtype width consistent with length.
 * (STRING: NUL present within bounds), BOOL in {0,1}.
 * Returns DECODER_OK iff frame_decode would not fail on structural grounds.
 * Does not allocate, does not touch a global.
 */
int frame_check(const uint8_t *payload, uint16_t length);

/*  Decode
 * Validates via frame_check, then fills `out` with a zero-copy view.
 * `out` is untouched on failure. The value is NOT converted to native order here;
 * callers that need conversion use the typed helpers below, STRING: out->len includes the NUL; out->data is NUL-terminated.
 */
int frame_decode(const uint8_t *payload, uint16_t length, decoder_data *out);

/*  Encode
 * Writes [dtype][flags][data] into `buf` (capacity `buf_max`).
 * `value`/`value_len` must be consistent with dtype:
 *   - fixed-width : value_len == width
 *   - STRING      : 1 <= value_len <= DECODER_STRING_MAX, value[value_len-1] == '\0'
 * Returns DECODER_OK and sets *written on success; nothing is written on failure.
 */
int frame_encode(decoder_dtype dtype, uint8_t flags,
                 const void *value, uint16_t value_len,
                 uint8_t *buf, uint16_t buf_max, uint16_t *written);

/*  Typed accessors
 * Decode a fixed-width value to native order. `src_big_endian` selects the
 * wire order for this call (ignored for 1-byte types). These do NOT read a
 * flags byte; the caller decides the wire order, which is the point.
 */
int decoder_get_u8(const uint8_t *data, uint16_t len, uint8_t  *out);
int decoder_get_u16(const uint8_t *data, uint16_t len, bool src_big_endian, uint16_t *out);
int decoder_get_u32(const uint8_t *data, uint16_t len, bool src_big_endian, uint32_t *out);
int decoder_get_i16(const uint8_t *data, uint16_t len, bool src_big_endian, int16_t  *out);
int decoder_get_i32(const uint8_t *data, uint16_t len, bool src_big_endian, int32_t  *out);
int decoder_get_bool(const uint8_t *data, uint16_t len, bool *out);

#endif /* CORE_LIB_DECODER_H */
