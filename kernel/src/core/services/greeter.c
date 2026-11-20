#include <stddef.h>
#include <stdint.h>
#include <core/services/router.h>

static const char t_ascii[] =
        "\n"
        "       ___             _     _       _\n"
        "      / __|_  _ _ __  | |__ (_) ___ | |_  ___\n"
        "      \\__ \\ || | '  \\ | '_ \\| |/ _ \\|  _|/ -_)\n"
        "      |___/\\_, |_|_|_||_.__/|_|\\___/ \\__|\\___|OS\n"
        "            |__/x86-64\n";

void init_header(void) {
    router_send(router_create(ADDR_NOBODY, ADDR_UNICAST(1), FRAME_DATA,
                              (const uint8_t *)t_ascii, (uint16_t)(sizeof(t_ascii) - 1u)));
}
