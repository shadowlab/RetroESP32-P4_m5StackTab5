/* Stubs that let src/io/gwenesis_io.c build on the host (see six_button_test.c). */
#include <stddef.h>
int fake_clock = 0;
int m68k_cycles_master(void) { return fake_clock; }
typedef struct SaveState SaveState;
SaveState* saveGwenesisStateOpenForWrite(const char* n) { (void)n; return 0; }
SaveState* saveGwenesisStateOpenForRead(const char* n) { (void)n; return 0; }
void saveGwenesisStateSetBuffer(SaveState* s, const char* t, void* b, size_t l) { (void)s; (void)t; (void)b; (void)l; }
void saveGwenesisStateGetBuffer(SaveState* s, const char* t, void* b, size_t l) { (void)s; (void)t; (void)b; (void)l; }
void gwenesis_io_get_buttons(void) {}
