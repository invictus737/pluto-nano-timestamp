/*
 * `abi`: the C layouts and constants Rust std and nexus2 depend on, evaluated by this compiler
 * with the board's headers, next to the value the vendored Rust libc crate (nexus2
 * vendor/libc, target_os = "rtems") uses. abi_table.h is generated from nexus2
 * scripts/rtems-abi/checks.txt; CI (rtos/ci/nexus2-abi.sh) fails when it is out of date or when
 * the crate disagrees with arm-rtems6-gcc. On the board this is the final confirmation.
 *   abi      mismatches only, then the count
 *   abi -v   every row
 */
#include <stdio.h>
#include <string.h>
#define ABI_INCLUDES
#include "abi_table.h"
#undef ABI_INCLUDES

int nexus_shell_abi(int argc, char **argv);

struct abi_row {
  const char *what;
  long long c;
  long long rust;
};

#define ABI_ROW(what, cexpr, rust) {what, (long long)(cexpr), rust},
static const struct abi_row abi_rows[] = {
#include "abi_table.h"
};
#undef ABI_ROW

int nexus_shell_abi(int argc, char **argv)
{
  int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
  unsigned n = sizeof(abi_rows) / sizeof(abi_rows[0]), bad = 0;
  for (unsigned i = 0; i < n; i++) {
    const struct abi_row *r = &abi_rows[i];
    /* a C constant may be unsigned where the crate has c_int: compare the low 32 bits too */
    int ok = r->c == r->rust || (unsigned)r->c == (unsigned)r->rust;
    bad += !ok;
    if (verbose || !ok) {
      printf("%-44s C %-12lld libc-crate %-12lld %s\n", r->what, r->c, r->rust, ok ? "ok" : "MISMATCH");
    }
  }
  printf("%u checks, %u mismatch(es)\n", n, bad);
  return bad != 0;
}
