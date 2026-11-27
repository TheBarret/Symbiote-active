#ifndef CORE_SERVICES_DECODER_H
#define CORE_SERVICES_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* codebase: 0.2.0 */

/* Frame Decoder
 * Frame  : FRAME_STRUCT
 * Layout : [ dtype:uint8_t | flags:uint8_t | data : length = dtype-width ]
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
    DTYPE_COUNT  = 8,   /* number of tags, not a type               */
} decoder_dtype;

/* flags, second byte of a structured payload */
#define DECODER_FLAG_BIG_ENDIAN  (1u << 0)  /* data is big-endian on the wire */

/* header size: dtype + flags */
#define DECODER_HEADER_SIZE  2u

/* max STRING payload, not counting the NUL */
#define DECODER_STRING_MAX   255u

enum decoder_result {
    DECODER_OK                    =  0,
    DECODER_ERR_INVALID           = -1,  /* generic fallback, unexpected */
    DECODER_ERR_ARG               = -2,  /* payload missing or null */
    DECODER_ERR_LENGTH            = -3,  /* length insufficient for type, or buffer too small */
    DECODER_ERR_TYPE              = -4,  /* no such type exists */
    DECODER_ERR_FLAG              = -5,  /* no such flag exists */
    DECODER_ERR_WIDTH             = -6,  /* width mismatch, or BOOL not 0/1 */
    DECODER_ERR_BAD_STR           = -7,  /* string malformed: no terminator, early NUL, too long */
};

/* decoded view: a validated typed value plus its type/flags.
 * `data` points into the original payload (zero copy) and is valid only for
 * the lifetime of the buffer it was decoded from.
 */
typedef struct {
    decoder_dtype dtype;
    uint8_t       flags;
    const uint8_t *data;    /* points into payload + DECODER_HEADER_SIZE */
    uint16_t      len;      /* value length in bytes (NUL included for STRING) */
} decoder_data;

/* Fixed-width types have a constant width; STRING is variable (DECODER_WIDTH_VAR).
 * DTYPE_NONE has width 0 and is always invalid.
 */
#define DECODER_WIDTH_VAR 0xFFFFu

typedef struct {
    uint16_t width;   /* fixed width, or DECODER_WIDTH_VAR */
    bool     valid;   /* DTYPE_NONE and reserved slots are false */
} decoder_tinfo;

/* NULL if dtype is out of range. */
const decoder_tinfo *decoder_typeinfo(decoder_dtype dtype);

/* Checks a raw structured payload against every known invariant without
 * interpreting it. Returns DECODER_OK iff frame_decode would not fail on
 * structural grounds. Does not allocate, does not touch a global.
 */
int frame_check(const uint8_t *payload, uint16_t length);

/* Validates via frame_check, then fills `out` with a zero-copy view.
 * `out` is untouched on failure. The value is NOT converted to native order;
 * use the typed accessors. STRING: out->len includes the NUL.
 */
int frame_decode(const uint8_t *payload, uint16_t length, decoder_data *out);

/* Writes [dtype][flags][data] into `buf` (capacity `buf_max`).
 *   - fixed-width : value_len == width
 *   - STRING      : 1 <= value_len <= DECODER_STRING_MAX + 1, ends with '\0', no embedded NUL
 * The value bytes are copied as given: DECODER_FLAG_BIG_ENDIAN is a label,
 * not a conversion, so the caller must already hold the value in wire order.
 * Returns DECODER_OK and sets *written on success; nothing is written on failure.
 */
int frame_encode(decoder_dtype dtype, uint8_t flags,
                 const void *value, uint16_t value_len,
                 uint8_t *buf, uint16_t buf_max, uint16_t *written);

/* Typed accessors: decode a fixed-width value to native order.
 * `src_big_endian` selects the wire order for this call (ignored for 1-byte
 * types). These do not read a flags byte; the caller decides the wire order.
 */
int decoder_get_u8(const uint8_t *data, uint16_t len, uint8_t  *out);
int decoder_get_u16(const uint8_t *data, uint16_t len, bool src_big_endian, uint16_t *out);
int decoder_get_u32(const uint8_t *data, uint16_t len, bool src_big_endian, uint32_t *out);
int decoder_get_i16(const uint8_t *data, uint16_t len, bool src_big_endian, int16_t  *out);
int decoder_get_i32(const uint8_t *data, uint16_t len, bool src_big_endian, int32_t  *out);
int decoder_get_bool(const uint8_t *data, uint16_t len, bool *out);

#endif /* CORE_SERVICES_DECODER_H */
