/* Calls the compiled kernels: `harness dot|matvec|sum|get <k>`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
float dot(int64_t n, float* a, float* b);
void matvec(int64_t m, int64_t n, float* a, float* x, float* y);
int64_t sum_to_n(int64_t n);
float get(int64_t n, int64_t k, float* a);
int main(int argc, char** argv) {
  if (argc < 2) return 64;
  if (!strcmp(argv[1], "dot")) {
    float a[100], b[100];
    for (int i = 0; i < 100; ++i) { a[i] = (float)(i + 1); b[i] = 2.0f; }
    printf("%.1f\n", dot(100, a, b));
  } else if (!strcmp(argv[1], "matvec")) {
    float a[3 * 4], x[4], y[3];
    for (int i = 0; i < 12; ++i) a[i] = (float)i;
    for (int j = 0; j < 4; ++j) x[j] = (float)(j + 1);
    matvec(3, 4, a, x, y);
    printf("%.1f %.1f %.1f\n", y[0], y[1], y[2]);
  } else if (!strcmp(argv[1], "sum")) {
    printf("%lld\n", (long long)sum_to_n(100));
  } else if (!strcmp(argv[1], "get") && argc > 2) {
    float a[4] = {10, 20, 30, 40};
    printf("%.1f\n", get(4, atoll(argv[2]), a));
  }
  return 0;
}
