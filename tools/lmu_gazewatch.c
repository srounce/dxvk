// Watches the LMU gaze shared-memory file and prints decoded samples.
//
// The OpenXR gaze layer (openxr-gaze-daemon) writes /dev/shm/lmu_gaze in the
// D3D11GazeShm layout, and the DXVK foveation fork reads the same file via
// Z:\dev\shm\lmu_gaze. This shows exactly what DXVK sees, so it tells apart
// "the layer isn't writing", "it's writing invalid samples" and "it's writing
// good gaze that DXVK isn't applying". A line is printed only when the sample
// changes. Two interleaved sequence counters mean two writers are racing on
// the file (each writer has its own seqlock counter).
//
//   uint32 sequence; uint32 valid; float lX,lY,rX,rY; double timestampNs;  (32 bytes)
//
// Build (libc only, no OpenXR needed):  cc tools/lmu_gazewatch.c -o gazewatch
// Run alongside the game:               ./gazewatch

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct Shm {
  uint32_t seq, valid;
  float    lx, ly, rx, ry;
  double   ts;
};

int main(void) {
  const char* path = "/dev/shm/lmu_gaze";
  struct Shm prev;
  memset(&prev, 0, sizeof prev);
  int first = 1;

  for (;;) {
    FILE* f = fopen(path, "rb");
    if (!f) {
      printf("(no %s yet)\n", path);
      sleep(1);
      continue;
    }

    struct Shm s;
    size_t n = fread(&s, 1, sizeof s, f);
    fclose(f);

    if (n == sizeof s) {
      int changed = first
        || s.lx != prev.lx || s.ly != prev.ly
        || s.rx != prev.rx || s.ry != prev.ry
        || s.valid != prev.valid;

      if (changed) {
        printf("seq=%u valid=%u L=(%+.3f,%+.3f) R=(%+.3f,%+.3f) %s\n",
          s.seq, s.valid, s.lx, s.ly, s.rx, s.ry, s.valid ? "" : "<invalid>");
        fflush(stdout);
        prev = s;
        first = 0;
      }
    }

    usleep(150000);
  }
}
