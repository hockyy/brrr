/* Runs the compiled kernel against torch's output.

     harness <data-dir> <t> <reps>

   Loads the parameters and the input written by kernel.py's dump_data, calls
   `forward` (the kernel Brrr compiled) `reps` times, and prints the largest
   absolute difference from torch's output and the time per call. Exits 1 when
   the difference exceeds 1e-4. */
#define _CRT_SECURE_NO_WARNINGS /* fopen on MSVC runtimes */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void forward(int64_t t, float* fc1_weight, float* fc1_bias, float* fc2_weight, float* fc2_bias,
             float* x, float* out);

static float* load(const char* dir, const char* name, size_t n) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s.bin", dir, name);
  FILE* f = fopen(path, "rb");
  float* p = malloc(n * sizeof(float));
  if (!f || !p || fread(p, sizeof(float), n, f) != n) {
    fprintf(stderr, "cannot read %s\n", path);
    exit(2);
  }
  fclose(f);
  return p;
}

static double now_ms(void) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: harness <data-dir> <t> <reps>\n");
    return 64;
  }
  const char* dir = argv[1];
  const int64_t t = atoll(argv[2]), tokens = 8 * t;
  const int reps = atoi(argv[3]);
  float* w1 = load(dir, "fc1_weight", 256 * 64);
  float* b1 = load(dir, "fc1_bias", 256);
  float* w2 = load(dir, "fc2_weight", 64 * 256);
  float* b2 = load(dir, "fc2_bias", 64);
  float* x = load(dir, "x", (size_t)tokens * 64);
  float* ref = load(dir, "y", (size_t)tokens * 64);
  float* out = calloc((size_t)tokens * 64, sizeof(float));

  forward(t, w1, b1, w2, b2, x, out); /* warm-up, and the result we check */
  double err = 0;
  for (int64_t i = 0; i < tokens * 64; ++i) err = fmax(err, fabs((double)out[i] - ref[i]));
  const double start = now_ms();
  for (int r = 0; r < reps; ++r) forward(t, w1, b1, w2, b2, x, out);
  const double ms = (now_ms() - start) / (reps > 0 ? reps : 1);
  printf("tokens=%lld max_abs_err=%.3g ms_per_call=%.4f\n", (long long)tokens, err, ms);
  return err <= 1e-4 ? 0 : 1;
}
