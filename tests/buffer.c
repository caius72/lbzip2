/*-
  buffer.c -- tests of the buffer-to-buffer interface

  This file is part of lbzip2.

  lbzip2 is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  lbzip2 is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with lbzip2.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
  Usage: buffer SUITE-DIR LEVELS
         buffer synthetic

  The lbzip2 tool in the current directory is the reference: whatever it
  accepts the interface must accept, with the same output, and whatever it
  rejects the interface must reject.  The tool itself is checked against
  minbzcat by the main suite.

  With a directory, every .bz2 file in it is decompressed, and what
  decompresses is compressed again at each of the LEVELS, a string of digits,
  both as the tool does by default and as it does with -u -- unless it is
  over 4 MB, as the zip bombs among the decompression tests are: compressing
  those again would take most of the time and test nothing new.  "synthetic"
  covers what the suite's small files do not: inputs of many blocks, so that
  threads have something to share, empty input, concatenated streams,
  trailing garbage, truncation and a full output buffer.

  Each case runs without an executor, on a pool of four threads, and on an
  executor that declines every other task, which the caller then runs
  itself.
*/

#include "../src/buffer.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


/* decode.c allocates through xmalloc(), which main.c defines for the tool. */
void *
xmalloc(size_t size)
{
  void *p = malloc(size);

  if (p == NULL)
    abort();
  return p;
}


static const char *current;     /* the case being run, for messages */
static unsigned failures;

static void
fail(const char *fmt, ...)
{
  va_list ap;

  fprintf(stderr, "FAIL %s: ", current);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  failures++;
}

static void *
xalloc(size_t size)
{
  void *p = malloc(size > 0 ? size : 1);

  if (p == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  return p;
}


struct buf {
  unsigned char *data;
  size_t size;
};

static void
append(struct buf *b, const void *data, size_t size)
{
  b->data = realloc(b->data, b->size + size + 1);
  if (b->data == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(2);
  }
  memcpy(b->data + b->size, data, size);
  b->size += size;
}

static struct buf
read_file(const char *path)
{
  struct buf b = { NULL, 0 };
  unsigned char chunk[65536];
  size_t n;
  FILE *f = fopen(path, "rb");

  if (f == NULL) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(2);
  }
  append(&b, "", 0);
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
    append(&b, chunk, n);
  fclose(f);
  return b;
}

static void
write_file(const char *path, const void *data, size_t size)
{
  FILE *f = fopen(path, "wb");

  if (f == NULL || fwrite(data, 1, size, f) != size || fclose(f) != 0) {
    fprintf(stderr, "cannot write %s\n", path);
    exit(2);
  }
}


/* Runs ./lbzip2 with the given options (option2 may be NULL) on the file at
   in_path, and collects what it writes.  Returns true if it succeeded. */
static int
run_tool2(const char *option, const char *option2, const char *in_path,
          struct buf *out)
{
  int fds[2];
  pid_t pid;
  int status;
  unsigned char chunk[65536];
  ssize_t n;

  out->data = NULL;
  out->size = 0;
  append(out, "", 0);

  if (pipe(fds) != 0) {
    perror("pipe");
    exit(2);
  }
  pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(2);
  }
  if (pid == 0) {
    int in_fd = open(in_path, O_RDONLY);
    int null_fd = open("/dev/null", O_WRONLY);

    if (in_fd < 0 || null_fd < 0)
      _exit(127);
    dup2(in_fd, 0);
    dup2(fds[1], 1);
    dup2(null_fd, 2);
    close(fds[0]);
    close(fds[1]);
    /* Its output does not depend on the number of threads, and starting one
       per core costs more than these small files take to compress. */
    execl("./lbzip2", "lbzip2", "-n4", option, option2, (char *)NULL);
    _exit(127);
  }

  close(fds[1]);
  while ((n = read(fds[0], chunk, sizeof chunk)) > 0)
    append(out, chunk, (size_t)n);
  close(fds[0]);
  if (waitpid(pid, &status, 0) != pid) {
    perror("waitpid");
    exit(2);
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    fprintf(stderr, "cannot run ./lbzip2\n");
    exit(2);
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
run_tool(const char *option, const char *in_path, struct buf *out)
{
  return run_tool2(option, NULL, in_path, out);
}


/* An executor that declines every other task, on top of a real one. */
struct declining {
  struct lbzip2_executor executor;
  const struct lbzip2_executor *inner;
  unsigned calls;
};

static void *
declining_submit(void *opaque, void (*fn)(void *), void *arg)
{
  struct declining *d = opaque;

  if (d->calls++ % 2 == 0)
    return NULL;
  return d->inner->submit(d->inner->opaque, fn, arg);
}

static void
declining_wait(void *opaque, void *handle)
{
  struct declining *d = opaque;

  d->inner->wait(d->inner->opaque, handle);
}


struct config {
  const char *name;
  unsigned threads;
  const struct lbzip2_executor *executor;
};

static struct declining declining;
static struct config configs[3];
static const size_t nconfigs = sizeof configs / sizeof configs[0];

/* With a single block, the declining executor declines it, and the caller
   runs it as it would without an executor; it takes a path of its own only
   when there are more blocks, which needs more than the smallest block. */
static int
skip_config(size_t i, size_t data_size)
{
  return configs[i].executor == &declining.executor && data_size <= 100000u;
}


/* Checks that every configuration decompresses z as the tool does: to
   expected if ok, and to an error if not. */
static void
check_expand(const struct buf *z, int ok, const struct buf *expected)
{
  size_t i;

  for (i = 0; i < nconfigs; i++) {
    /* What the tool rejects may still decompress to a lot before it is
       found out, so leave room for that rather than run out of it. */
    size_t cap = ok ? expected->size : expected->size + (64u << 20);
    unsigned char *out;
    size_t out_size = cap;
    int rv;

    if (ok && skip_config(i, expected->size))
      continue;
    out = xalloc(cap);
    rv = lbzip2_expand_buffer(z->data, z->size, out, &out_size,
                              configs[i].threads, configs[i].executor);

    if (ok && rv != LBZIP2_OK)
      fail("%s: expand returned %d, the tool succeeded", configs[i].name, rv);
    else if (ok && (out_size != expected->size ||
                    memcmp(out, expected->data, out_size) != 0))
      fail("%s: expand output differs from the tool's", configs[i].name);
    else if (!ok && rv == LBZIP2_OK)
      fail("%s: expand succeeded, the tool failed", configs[i].name);

    /* One byte too few must be reported, not overrun.  The declining
       executor takes no path the other two do not. */
    if (ok && expected->size > 0 && configs[i].executor != &declining.executor) {
      out_size = expected->size - 1;
      rv = lbzip2_expand_buffer(z->data, z->size, out, &out_size,
                                configs[i].threads, configs[i].executor);
      if (rv != LBZIP2_ERR_OUTPUT)
        fail("%s: expand into a short buffer returned %d", configs[i].name,
             rv);
    }
    free(out);
  }
}

/* Checks that every configuration compresses raw at level with flags to what
   the tool writes, which is passed as expected. */
static void
check_compress(const struct buf *raw, int level, int flags,
               const struct buf *expected)
{
  size_t i;

  for (i = 0; i < nconfigs; i++) {
    size_t cap = expected->size;
    unsigned char *out;
    size_t out_size = cap;
    const char *u = (flags & LBZIP2_SEQUENTIAL) ? " -u" : "";
    int rv;

    if (skip_config(i, raw->size))
      continue;
    out = xalloc(cap);
    rv = lbzip2_compress_buffer(raw->data, raw->size, out, &out_size,
                                level, flags, configs[i].threads,
                                configs[i].executor);

    if (rv != LBZIP2_OK)
      fail("%s: compress -%d%s returned %d", configs[i].name, level, u, rv);
    else if (out_size != expected->size ||
             memcmp(out, expected->data, out_size) != 0)
      fail("%s: compress -%d%s output differs from the tool's",
           configs[i].name, level, u);

    if (configs[i].executor != &declining.executor) {
      out_size = cap - 1;
      rv = lbzip2_compress_buffer(raw->data, raw->size, out, &out_size,
                                  level, flags, configs[i].threads,
                                  configs[i].executor);
      if (rv != LBZIP2_ERR_OUTPUT)
        fail("%s: compress -%d%s into a short buffer returned %d",
             configs[i].name, level, u, rv);
    }
    free(out);
  }
}

static void
check_raw(const struct buf *raw, const char *tmp_path, const int *levels,
          size_t nlevels)
{
  size_t i;

  write_file(tmp_path, raw->data, raw->size);
  for (i = 0; i < nlevels; i++) {
    char option[3] = { '-', (char)('0' + levels[i]), 0 };
    int flags;

    for (flags = 0; flags <= LBZIP2_SEQUENTIAL; flags += LBZIP2_SEQUENTIAL) {
      struct buf z;

      /* Input that is a single piece is compressed the same either way. */
      if (flags != 0 && raw->size <= levels[i] * 100000u)
        continue;

      if (!run_tool2(option, flags ? "-u" : NULL, tmp_path, &z)) {
        fail("the tool failed to compress at %s", option);
        free(z.data);
        continue;
      }
      check_compress(raw, levels[i], flags, &z);
      check_expand(&z, 1, raw);
      free(z.data);
    }
  }
}

static void
check_file(const char *path, const char *tmp_path, const int *levels,
           size_t nlevels)
{
  struct buf z = read_file(path);
  struct buf raw;
  int ok = run_tool("-dc", path, &raw);

  check_expand(&z, ok, &raw);
  if (ok && raw.size <= (4u << 20))
    check_raw(&raw, tmp_path, levels, nlevels);
  free(z.data);
  free(raw.data);
}


static int
run_suite(const char *dir, const char *level_digits)
{
  int levels[9];
  size_t nlevels = 0;
  DIR *d = opendir(dir);
  struct dirent *e;
  const char *base = strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir;
  char *tmp_path = xalloc(strlen(base) + 32);
  unsigned count = 0;

  if (d == NULL) {
    fprintf(stderr, "cannot open %s\n", dir);
    return 2;
  }
  sprintf(tmp_path, "buffer-%s.tmp", base);
  for (; *level_digits != 0 && nlevels < 9; level_digits++) {
    if (*level_digits < '1' || *level_digits > '9') {
      fprintf(stderr, "levels are digits from 1 to 9\n");
      return 2;
    }
    levels[nlevels++] = *level_digits - '0';
  }

  while ((e = readdir(d)) != NULL) {
    size_t len = strlen(e->d_name);
    char *path;

    if (len < 5 || strcmp(e->d_name + len - 4, ".bz2") != 0)
      continue;
    path = xalloc(strlen(dir) + len + 2);
    sprintf(path, "%s/%s", dir, e->d_name);
    current = path;
    check_file(path, tmp_path, levels, nlevels);
    free(path);
    count++;
  }
  closedir(d);
  remove(tmp_path);
  free(tmp_path);

  printf("%u files, %u failures\n", count, failures);
  return count == 0 || failures > 0;
}


/* Deterministic data that compresses somewhat, so blocks are full. */
static struct buf
make_data(size_t size, uint32_t seed)
{
  static const char *const words[] = {
    "lbzip2 ", "block ", "thread ", "stream ", "the ", "of ", "and ",
    "Burrows-Wheeler ", "transform ", "\n", "0123456789 ", "abcdefgh ",
  };
  struct buf b = { NULL, 0 };
  uint32_t x = seed;

  append(&b, "", 0);
  while (b.size < size) {
    const char *w;
    unsigned char c;

    x = x * 1103515245u + 12345u;
    if ((x >> 16) % 16 == 0) {
      /* Now and then a stretch of noise, or a long run. */
      size_t n = 1 + (x >> 20) % 64, i;

      for (i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        c = (unsigned char)(x >> 24);
        append(&b, &c, 1);
      }
      if ((x >> 8) % 4 == 0)
        for (i = 0; i < 300; i++)
          append(&b, "z", 1);
      continue;
    }
    w = words[(x >> 16) % (sizeof words / sizeof words[0])];
    append(&b, w, strlen(w));
  }
  b.size = size;
  return b;
}

static int
run_synthetic(void)
{
  static const int all_levels[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  static const int one_level[] = { 1 };
  const char *tmp_path = "buffer-synthetic.tmp";
  struct buf empty = { NULL, 0 };
  struct buf big = make_data(1500000, 1);
  struct buf small = make_data(1000, 2);
  struct buf z, z2, cut;
  size_t i;

  /* Many blocks at every level: fifteen at level 1, two at level 9. */
  current = "1.5 MB";
  check_raw(&big, tmp_path, all_levels, 9);

  current = "empty input";
  append(&empty, "", 0);
  check_raw(&empty, tmp_path, one_level, 1);

  current = "one byte";
  {
    struct buf one = { NULL, 0 };

    append(&one, "x", 1);
    check_raw(&one, tmp_path, all_levels, 9);
    free(one.data);
  }

  /* Two streams, then the same with garbage after them, then cut short at
     every length within the last block's end: each must be accepted or
     rejected as the tool does. */
  current = "concatenated streams";
  write_file(tmp_path, big.data, big.size);
  if (!run_tool("-1", tmp_path, &z)) {
    fail("the tool failed to compress");
    return 1;
  }
  {
    struct buf zz = { NULL, 0 }, expected;
    int ok;

    append(&zz, z.data, z.size);
    write_file(tmp_path, small.data, small.size);
    run_tool("-9", tmp_path, &z2);
    append(&zz, z2.data, z2.size);
    write_file(tmp_path, zz.data, zz.size);
    ok = run_tool("-dc", tmp_path, &expected);
    check_expand(&zz, ok, &expected);
    free(expected.data);

    current = "trailing garbage";
    append(&zz, "garbage", 7);
    write_file(tmp_path, zz.data, zz.size);
    ok = run_tool("-dc", tmp_path, &expected);
    check_expand(&zz, ok, &expected);
    free(expected.data);
    free(zz.data);
    free(z2.data);
  }

  /* Garbage that is made of blocks, a stream without its stream header: the
     search for block headers finds them, and they must all be thrown away,
     as the tool ignores them. */
  current = "trailing blocks";
  {
    struct buf zz = { NULL, 0 }, expected;
    int ok;

    append(&zz, z.data, z.size);
    append(&zz, z.data + 4, z.size - 4);
    write_file(tmp_path, zz.data, zz.size);
    ok = run_tool("-dc", tmp_path, &expected);
    check_expand(&zz, ok, &expected);
    free(expected.data);
    free(zz.data);
  }

  current = "truncated";
  for (i = 1; i <= 16 && i < z.size; i++) {
    struct buf expected;
    int ok;

    cut.data = z.data;
    cut.size = z.size - i;
    write_file(tmp_path, cut.data, cut.size);
    ok = run_tool("-dc", tmp_path, &expected);
    check_expand(&cut, ok, &expected);
    free(expected.data);
  }

  current = "corrupted";
  for (i = 0; i < 8; i++) {
    struct buf bad = { NULL, 0 }, expected;
    int ok;

    append(&bad, z.data, z.size);
    bad.data[z.size / 2 + i * 977] ^= (unsigned char)(1u << i);
    write_file(tmp_path, bad.data, bad.size);
    ok = run_tool("-dc", tmp_path, &expected);
    check_expand(&bad, ok, &expected);
    free(expected.data);
    free(bad.data);
  }

  remove(tmp_path);
  free(z.data);
  free(big.data);
  free(small.data);
  free(empty.data);

  printf("%u failures\n", failures);
  return failures > 0;
}


int
main(int argc, char **argv)
{
  struct lbzip2_executor *pool;
  int rv;

  if (!(argc == 2 && strcmp(argv[1], "synthetic") == 0) && argc != 3) {
    fprintf(stderr, "usage: buffer SUITE-DIR LEVELS | buffer synthetic\n");
    return 2;
  }

  pool = lbzip2_pthread_executor_create(4);
  if (pool == NULL) {
    fprintf(stderr, "cannot start threads\n");
    return 2;
  }
  declining.executor.submit = declining_submit;
  declining.executor.wait = declining_wait;
  declining.executor.opaque = &declining;
  declining.inner = pool;
  declining.calls = 0;

  configs[0].name = "no executor";
  configs[0].threads = 1;
  configs[0].executor = NULL;
  configs[1].name = "4 threads";
  configs[1].threads = 4;
  configs[1].executor = pool;
  configs[2].name = "declining executor";
  configs[2].threads = 3;
  configs[2].executor = &declining.executor;

  if (argc == 2)
    rv = run_synthetic();
  else
    rv = run_suite(argv[1], argv[2]);

  lbzip2_pthread_executor_destroy(pool);
  return rv;
}
