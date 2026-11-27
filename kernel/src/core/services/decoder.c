#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/mem.h>
#include <core/services/decoder.h>

_Static_assert(DTYPE_STRING + 1 == DTYPE_COUNT, "decoder: DTYPE_COUNT out of step with the type list");

static const decoder_tinfo g_types[DTYPE_COUNT] = {
    [DTYPE_NONE]   = { .width = 0,                 .valid = false },
    [DTYPE_BYTE]   = { .width = 1,                 .valid = true  },
    [DTYPE_INT16]  = { .width = 2,                 .valid = true  },
    [DTYPE_INT32]  = { .width = 4,                 .valid = true  },
    [DTYPE_UINT16] = { .width = 2,                 .valid = true  },
    [DTYPE_UINT32] = { .width = 4,                 .valid = true  },
    [DTYPE_BOOL]   = { .width = 1,                 .valid = true  },
    [DTYPE_STRING] = { .width = DECODER_WIDTH_VAR, .valid = true  },
};

const decoder_tinfo *decoder_typeinfo(decoder_dtype dtype) {
    if ((unsigned)dtype >= (unsigned)DTYPE_COUNT) return NULL;
    return &g_types[dtype];
}

int frame_check(const uint8_t *payload, uint16_t length) {
    if (!payload) return DECODER_ERR_ARG;
    if (length < DECODER_HEADER_SIZE) return DECODER_ERR_LENGTH;

    decoder_dtype dtype = (decoder_dtype)payload[0];
    uint8_t       flags = payload[1];
    const uint8_t *data = payload + DECODER_HEADER_SIZE;
    uint16_t      dlen  = (uint16_t)(length - DECODER_HEADER_SIZE);

    const decoder_tinfo *ti = decoder_typeinfo(dtype);
    if (!ti || !ti->valid) return DECODER_ERR_TYPE;
    if (flags & (uint8_t)~DECODER_FLAG_BIG_ENDIAN) return DECODER_ERR_FLAG;

    if (ti->width != DECODER_WIDTH_VAR) {
        if (dlen != ti->width) return DECODER_ERR_WIDTH;
        if (dtype == DTYPE_BOOL && data[0] > 1) return DECODER_ERR_WIDTH;
        return DECODER_OK;
    }

    /* STRING: bounded, the only NUL is the last byte */
    if (dlen == 0 || dlen > DECODER_STRING_MAX + 1u) return DECODER_ERR_BAD_STR;
    for (uint16_t i = 0; i < dlen; i++) {
        if (data[i] == 0) return (i == dlen - 1u) ? DECODER_OK : DECODER_ERR_BAD_STR;
    }
    return DECODER_ERR_BAD_STR;   /* no terminator within the payload */
}

int frame_decode(const uint8_t *payload, uint16_t length, decoder_data *out) {
    if (!out) return DECODER_ERR_ARG;
    int rc = frame_check(payload, length);
    if (rc != DECODER_OK) return rc;

    out->dtype = (decoder_dtype)payload[0];
    out->flags = payload[1];
    out->data  = payload + DECODER_HEADER_SIZE;
    out->len   = (uint16_t)(length - DECODER_HEADER_SIZE);
    return DECODER_OK;
}

int frame_encode(decoder_dtype dtype, uint8_t flags,
                 const void *value, uint16_t value_len,
                 uint8_t *buf, uint16_t buf_max, uint16_t *written) {
    if (!buf || !written) return DECODER_ERR_ARG;

    const decoder_tinfo *ti = decoder_typeinfo(dtype);
    if (!ti || !ti->valid) return DECODER_ERR_TYPE;

    if (flags & (uint8_t)~DECODER_FLAG_BIG_ENDIAN) return DECODER_ERR_FLAG;

    if (!value || value_len == 0) return DECODER_ERR_LENGTH;

    if (ti->width != DECODER_WIDTH_VAR) {
        if (value_len != ti->width) return DECODER_ERR_WIDTH;
        if (dtype == DTYPE_BOOL) {
            uint8_t b;
            memcpy(&b, value, 1);
            if (b > 1) return DECODER_ERR_WIDTH;
        }
    } else {
        /* STRING: bounded, NUL-terminated, no embedded NUL */
        if (value_len > DECODER_STRING_MAX + 1u) return DECODER_ERR_BAD_STR;
        const uint8_t *s = (const uint8_t *)value;
        if (s[value_len - 1] != 0) return DECODER_ERR_BAD_STR;
        for (uint16_t i = 0; i < value_len - 1u; i++) {
            if (s[i] == 0) return DECODER_ERR_BAD_STR;
        }
    }

    /* capacity: header + data */
    size_t total = (size_t)DECODER_HEADER_SIZE + value_len;
    if (total > buf_max) return DECODER_ERR_LENGTH;

    buf[0] = (uint8_t)dtype;
    buf[1] = flags;
    memcpy(buf + DECODER_HEADER_SIZE, value, value_len);

    *written = (uint16_t)total;
    return DECODER_OK;
}
