#ifndef EXT_SCRIPT_TEST_H
#define EXT_SCRIPT_TEST_H

#include <stddef.h>
#include <stdint.h>

int dec_selftest(void);
int vm16_selftest(void);
int vm16_decimal_integration_test(void);
int vm16_decimal_fault_test(void);
int vm16_decimal_setget_test(void);

#endif
