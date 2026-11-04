#include "cesdk.h"

// 5.0 smoke: prove the SDK runtime reaches the console and the stop device
// on both boards before the boot protocol exists.
int main(void) {
  Puts("cesdk hello\n");
  Puts("  arch=");
#if defined(__riscv)
  Puts("riscv64");
#elif defined(__i386__)
  Puts("x86");
#else
  Puts("?");
#endif
  Puts(" build=5.0\n");
  Puts("  0x");
  Puthex(0xcafe1234);
  Puts(" ");
  Putdec(1234567890);
  Puts("\n");
  return 0;
}
