/*
 * Host test for the 6-button pad protocol in src/io/gwenesis_io.c.
 * Drives port 1 the way a game's read routine does and checks every response.
 *
 *   ./run_six_button_test.sh
 */
#include <stdio.h>
#include "gwenesis_io.h"
enum { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_B, PAD_C, PAD_A, PAD_S };
extern int fake_clock;
static int fails;
/* Port 1: data register 0xA10003 -> address 0x03, ctrl 0xA10009 -> 0x09 (gwenesis shifts >>1) */
static void th(int v) { gwenesis_io_write_ctrl(0x03, v ? 0x40 : 0x00); fake_clock += 100; }
static unsigned rd(void) { fake_clock += 20; return gwenesis_io_read_ctrl(0x03) & 0x7f; }
static void expect(const char *what, unsigned got, unsigned want) {
    printf("  %-28s got %02X want %02X %s\n", what, got, want, got == want ? "ok" : "FAIL");
    if (got != want) fails++;
}
/* Typical 6-button read routine: TH 1,0,1,0,1,0,1,0 */
static void sequence(const char *title, unsigned w[8]) {
    printf("%s\n", title);
    const char *n[8] = {"TH=1 #1 ?1CBRLDU", "TH=0 #1 ?0SA00DU", "TH=1 #2", "TH=0 #2", "TH=1 #3", "TH=0 #3 ID ?0SA0000", "TH=1 #4 ?1CBMXYZ", "TH=0 #4 ?0SA1111"};
    for (int i = 0; i < 8; i++) { th(!(i & 1)); expect(n[i], rd(), w[i]); }
}
int main(void) {
    gwenesis_io_write_ctrl(0x09, 0x40);           /* TH as output */
    /* press A, C, X, MODE (active low bits) */
    gwenesis_io_pad_press_button(0, PAD_A); gwenesis_io_pad_press_button(0, PAD_C);
    gwenesis_io_pad_press_button(0, PAD_X); gwenesis_io_pad_press_button(0, PAD_MODE);
    /* bits: TH=1 -> 0x40 | C B R L D U ; C pressed -> bit5 = 0 => 0x5F
             TH=0 -> S A 0 0 D U ; A pressed -> bit4 = 0 => 0x23
             ext: M X Y Z ; X,M pressed -> 0b0011 => TH=1 #4 = 0x40|0x10(B)|0x03 = 0x53 */
    unsigned three[8] = {0x5F,0x23,0x5F,0x23,0x5F,0x23,0x5F,0x23};
    sequence("3-button pad (default): never answers the 6-button reads", three);
    gwenesis_io_set_pad_type(0, GWENESIS_PAD_6BUTTON);
    fake_clock += 200000;                           /* idle > timeout */
    unsigned six[8] = {0x5F,0x23,0x5F,0x23,0x5F,0x20,0x53,0x2F};
    sequence("6-button pad", six);
    fake_clock += 200000;
    sequence("6-button pad, next frame after timeout", six);
    printf("frame rebase: sequence split across gwenesis_io_frame_end()\n");
    fake_clock += 200000;
    for (int i = 0; i < 4; i++) { th(!(i & 1)); rd(); }
    gwenesis_io_frame_end(fake_clock - 50); fake_clock = 50;   /* rebase like genesis_run */
    th(1); expect("TH=1 #3 after rebase", rd(), 0x5F);
    th(0); expect("TH=0 #3 ID after rebase", rd(), 0x20);
    printf("no timeout: a 3rd read burst within 1.5 ms continues the count\n");
    fake_clock += 200000;
    for (int i = 0; i < 8; i++) { th(!(i & 1)); rd(); }
    th(1); expect("TH=1 #5 normal", rd(), 0x5F);
    th(0); expect("TH=0 #5 normal (capped)", rd(), 0x23);
    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails != 0;
}
