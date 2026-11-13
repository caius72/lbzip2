/*-
  buffer.c -- buffer-to-buffer compression and decompression

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

#include "common.h"

#include "buffer.h"
#include "decode.h"             /* parse(), retrieve(), decode(), emit() */
#include "encode.h"             /* collect(), encode(), transmit() */

#include <arpa/inet.h>          /* ntohl() */
#include <string.h>             /* memcpy() */


/*
  Both directions keep a window of blocks in flight, in input order.  The
  calling thread does the part of each block that has to follow the previous
  one, hands the rest to the executor, and copies finished blocks to the
  output from the oldest on, so the output is the same for any number of
  threads.  A task the executor declines runs on the calling thread at once,
  and without an executor every task does, one block at a time.
*/

static size_t
window_size(unsigned threads, const struct lbzip2_executor *executor)
{
  return executor != NULL && threads > 1 ? 2u * (size_t)threads : 1u;
}

static void *
start(const struct lbzip2_executor *executor, void (*fn)(void *), void *arg)
{
  void *handle = NULL;

  if (executor != NULL)
    handle = executor->submit(executor->opaque, fn, arg);
  if (handle == NULL)
    fn(arg);
  return handle;
}

static void
finish(const struct lbzip2_executor *executor, void *handle)
{
  if (handle != NULL)
    executor->wait(executor->opaque, handle);
}


/* COMPRESSION

   collect() fills a block, and only it knows where the block ends, as that
   depends on the run-length coding of the input before it; it is cheap.  It
   is given the input a piece at a time, as compress.c is given it: a piece
   of level * 100k bytes, or the whole input with LBZIP2_SEQUENTIAL.
   encode() and transmit() are nearly all of the work, and run as the task.
   The encoder pads every block to a whole number of bytes, so blocks are
   concatenated without shifting any bits. */

struct enc_job {
  struct encoder_state *enc;
  void *handle;
  size_t size;
  uint32_t crc;
  const void *block;
};

static void
enc_task(void *arg)
{
  struct enc_job *job = arg;

  job->size = encode(job->enc, &job->crc);
  job->block = transmit(job->enc, NULL);
}

int
lbzip2_compress_buffer(const void *in, size_t in_size,
                       void *out, size_t *out_size,
                       int level, int flags, unsigned threads,
                       const struct lbzip2_executor *executor)
{
  const uint8_t *ip = in;
  uint8_t *op = out;
  unsigned long mbs;
  struct enc_job *jobs;
  size_t nslots, head, count, left, piece, piece_left, pos, cap, i;
  uint32_t combined_crc;
  int rv;

  if (level < 1 || level > 9 || (flags & ~LBZIP2_SEQUENTIAL) != 0 ||
      out_size == NULL ||
      (in == NULL && in_size > 0) || (out == NULL && *out_size > 0))
    return LBZIP2_ERR_PARAM;
  cap = *out_size;
  if (cap < HEADER_SIZE + TRAILER_SIZE)
    return LBZIP2_ERR_OUTPUT;

  mbs = level * 100000ul;
  piece = (flags & LBZIP2_SEQUENTIAL) ? in_size : mbs;
  nslots = window_size(threads, executor);
  jobs = calloc(nslots, sizeof *jobs);
  if (jobs == NULL)
    return LBZIP2_ERR_MEMORY;

  op[0] = 0x42;
  op[1] = 0x5A;
  op[2] = 0x68;
  op[3] = 0x30 + level;
  pos = HEADER_SIZE;
  combined_crc = 0;
  left = in_size;
  piece_left = 0;
  head = 0;
  count = 0;
  rv = LBZIP2_OK;

  for (;;) {
    struct enc_job *job;

    if (rv == LBZIP2_OK && left > 0 && count < nslots) {
      job = &jobs[(head + count) % nslots];
      if (job->enc == NULL) {
        job->enc = malloc(encoder_alloc_size(mbs));
        if (job->enc == NULL) {
          rv = LBZIP2_ERR_MEMORY;
          continue;
        }
      }
      encoder_init(job->enc, mbs, CLUSTER_FACTOR);
      if (piece_left == 0)
        piece_left = left < piece ? left : piece;
      /* One call fills the block, or exhausts the piece trying. */
      left -= piece_left;
      (void)collect(job->enc, ip + (in_size - left - piece_left), &piece_left);
      left += piece_left;
      job->handle = start(executor, enc_task, job);
      count++;
      continue;
    }
    if (count == 0)
      break;

    job = &jobs[head];
    finish(executor, job->handle);
    head = (head + 1) % nslots;
    count--;

    if (rv != LBZIP2_OK)
      continue;
    /* pos + TRAILER_SIZE <= cap holds throughout. */
    if (job->size > cap - TRAILER_SIZE - pos) {
      rv = LBZIP2_ERR_OUTPUT;
      continue;
    }
    memcpy(op + pos, job->block, job->size);
    pos += job->size;
    combined_crc = combine_crc(combined_crc, job->crc);
  }

  for (i = 0; i < nslots; i++)
    free(jobs[i].enc);
  free(jobs);
  if (rv != LBZIP2_OK)
    return rv;

  op[pos++] = 0x17;
  op[pos++] = 0x72;
  op[pos++] = 0x45;
  op[pos++] = 0x38;
  op[pos++] = 0x50;
  op[pos++] = 0x90;
  op[pos++] = combined_crc >> 24;
  op[pos++] = (combined_crc >> 16) & 0xFF;
  op[pos++] = (combined_crc >> 8) & 0xFF;
  op[pos++] = combined_crc & 0xFF;

  *out_size = pos;
  return LBZIP2_OK;
}


/* DECOMPRESSION

   Where a block ends is only known once retrieve() has decoded all of its
   Huffman codes, and the next block starts there.  So, as expand.c does,
   this first looks for blocks by the 48-bit magic number that starts every
   block header, which scan() finds anywhere in the input, and then
   decompresses the blocks it found as tasks: retrieve(), decode() and as
   much of emit() as a buffer of the block's own takes.

   The calling thread meanwhile walks the stream with parse(), from one block
   header to the next, as a sequential decompressor would.  A task's result
   is used only if it started exactly where the walk finds the next block;
   retrieve() reads nothing but the bits from there on, so the result is the
   one the walk would have got itself.  Tasks that started at a magic number
   that occurred by chance inside other data are discarded, and a block the
   scan missed -- a chance magic number overlapping the real one -- is
   decompressed by the calling thread.  What is accepted and what is rejected
   is therefore the same as without threads. */

/* A bit stream that starts pos bits into words. */
static struct bitstream
bits_at(const uint32_t *words, size_t nwords, uint64_t pos)
{
  struct bitstream bs;
  size_t i = (size_t)(pos / 32u);
  unsigned skip = (unsigned)(pos % 32u);

  bs.block = NULL;
  bs.limit = words + nwords;
  bs.eof = true;
  if (i >= nwords) {
    bs.live = 0;
    bs.buff = 0;
    bs.data = bs.limit;
  }
  else {
    bs.live = 32u - skip;
    bs.buff = (uint64_t)ntohl(words[i]) << (32u + skip);
    bs.data = words + i + 1;
  }
  return bs;
}

/* How many bits of words the bit stream has read. */
static uint64_t
bits_pos(const struct bitstream *bs, const uint32_t *words)
{
  return 32u * (uint64_t)(bs->data - words) - bs->live;
}

/* parse() gives back the bits it read after the last stream; the stream must
   not have reached into the zero padding, or the input was cut short. */
static int
check_end(const struct bitstream *bs, const uint32_t *words,
          unsigned garbage, size_t payload_size)
{
  if (bits_pos(bs, words) - garbage > 8u * (uint64_t)payload_size)
    return LBZIP2_ERR_DATA;
  return LBZIP2_OK;
}

/* Emits the rest of a decoded block into out, after *produced bytes. */
static int
emit_rest(struct decoder_state *ds, uint8_t *out, size_t cap,
          size_t *produced)
{
  int rv;

  do {
    size_t avail = cap - *produced;

    if (avail == 0)
      return LBZIP2_ERR_OUTPUT;
    rv = emit(ds, out + *produced, &avail);
    *produced = cap - avail;
  } while (rv == MORE);

  return rv == OK ? LBZIP2_OK : LBZIP2_ERR_DATA;
}

/* Decompresses the block whose header parse() has just read, from the bit
   stream itself, straight into out. */
static int
expand_here(const struct header *hd, struct bitstream *bs, uint8_t *out,
            size_t cap, size_t *produced)
{
  struct decoder_state ds;
  int rv;

  decoder_init(&ds);
  /* retrieve() takes blocks of up to MAX_BLOCK_SIZE; the block size in the
     stream header is the real limit, as expand.c holds it. */
  if (retrieve(&ds, bs) != OK ||
      ds.block_size > (unsigned)hd->bs100k * 100000u)
    rv = LBZIP2_ERR_DATA;
  else {
    decode(&ds);
    rv = emit_rest(&ds, out, cap, produced);
    if (rv == LBZIP2_OK && ds.crc != hd->crc)
      rv = LBZIP2_ERR_DATA;
  }
  decoder_free(&ds);
  return rv;
}


/* Scanning one stretch of the input for block headers. */
struct scan_job {
  const uint32_t *words;
  size_t nwords;
  size_t first, last;           /* the words where a header may start */
  uint64_t *starts;             /* where the blocks found start, in bits */
  size_t count, size;
  int status;
  void *handle;
};

static void
scan_task(void *arg)
{
  struct scan_job *job = arg;
  struct bitstream bs;
  /* A header is 48 bits of magic number and 32 of CRC, which scan() reads
     past, so let it read up to three words beyond the stretch. */
  size_t limit = job->last + 3u < job->nwords ? job->last + 3u : job->nwords;

  bs.live = 0;
  bs.buff = 0;
  bs.block = NULL;
  bs.data = job->words + job->first;
  bs.limit = job->words + limit;
  bs.eof = true;

  while (scan(&bs, 0) == OK) {
    uint64_t start = bits_pos(&bs, job->words);

    if (start - 80u >= 32u * (uint64_t)job->last)
      break;
    if (job->count == job->size) {
      size_t size = job->size ? 2u * job->size : 64u;
      uint64_t *p = realloc(job->starts, size * sizeof *p);

      if (p == NULL) {
        job->status = LBZIP2_ERR_MEMORY;
        return;
      }
      job->starts = p;
      job->size = size;
    }
    job->starts[job->count++] = start;
  }
}

/* Finds where blocks may start, in bits after the end of each block header,
   in increasing order. */
static int
find_blocks(const uint32_t *words, size_t nwords, unsigned threads,
            const struct lbzip2_executor *executor, uint64_t **starts,
            size_t *count)
{
  /* A megabyte at a time, but as many stretches as threads at least. */
  size_t stretch = (size_t)1 << 18;
  size_t njobs, i, total = 0;
  struct scan_job *jobs;
  int rv = LBZIP2_OK;

  if (nwords / threads < stretch)
    stretch = nwords / threads + 1u;
  njobs = (nwords + stretch - 1) / stretch;
  jobs = calloc(njobs > 0 ? njobs : 1, sizeof *jobs);
  if (jobs == NULL)
    return LBZIP2_ERR_MEMORY;

  for (i = 0; i < njobs; i++) {
    jobs[i].words = words;
    jobs[i].nwords = nwords;
    jobs[i].first = i * stretch;
    jobs[i].last = (i + 1) * stretch < nwords ? (i + 1) * stretch : nwords;
    jobs[i].handle = start(executor, scan_task, &jobs[i]);
  }
  for (i = 0; i < njobs; i++) {
    finish(executor, jobs[i].handle);
    if (jobs[i].status != LBZIP2_OK)
      rv = jobs[i].status;
    total += jobs[i].count;
  }

  *starts = NULL;
  *count = 0;
  if (rv == LBZIP2_OK) {
    *starts = malloc((total > 0 ? total : 1) * sizeof **starts);
    if (*starts == NULL)
      rv = LBZIP2_ERR_MEMORY;
  }
  for (i = 0; i < njobs; i++) {
    if (rv == LBZIP2_OK && jobs[i].count > 0) {
      memcpy(*starts + *count, jobs[i].starts,
             jobs[i].count * sizeof **starts);
      *count += jobs[i].count;
    }
    free(jobs[i].starts);
  }
  free(jobs);
  return rv;
}


/* Decompressing a block that may start at a given bit. */
struct dec_job {
  const uint32_t *words;
  size_t nwords;
  uint64_t start;               /* bits after the end of the header */
  void *handle;
  struct decoder_state ds;
  bool live;                    /* ds needs decoder_free() */
  int retrieved;                /* what retrieve() returned */
  uint64_t end;                 /* where retrieve() stopped, in bits */
  unsigned block_size;
  int emitted;                  /* what emit() last returned */
  uint32_t crc;                 /* the block's CRC, once fully emitted */
  int status;                   /* LBZIP2_ERR_MEMORY, if it ran out */
  uint8_t *buf;                 /* what was emitted, kept for reuse */
  size_t len, size;
};

static void
dec_task(void *arg)
{
  struct dec_job *job = arg;
  struct bitstream bs = bits_at(job->words, job->nwords, job->start);
  size_t max;

  job->len = 0;
  job->emitted = MORE;
  job->status = LBZIP2_OK;
  decoder_init(&job->ds);
  job->live = true;

  job->retrieved = retrieve(&job->ds, &bs);
  job->end = bits_pos(&bs, job->words);
  job->block_size = job->ds.block_size;
  if (job->retrieved != OK) {
    decoder_free(&job->ds);
    job->live = false;
    return;
  }
  decode(&job->ds);

  /* Most blocks decompress to about their own size.  One that comes to more
     than twice that is finished by the calling thread, straight into the
     output, so that the blocks in flight hold a bounded amount of memory. */
  max = 2u * (size_t)job->block_size + 65536u;
  for (;;) {
    size_t avail;

    if (job->len == job->size) {
      size_t size;
      uint8_t *p;

      if (job->size >= max)
        return;
      size = job->size ? 2u * job->size
                       : job->block_size + job->block_size / 4u + 4096u;
      if (size > max)
        size = max;
      p = realloc(job->buf, size);
      if (p == NULL) {
        job->status = LBZIP2_ERR_MEMORY;
        return;
      }
      job->buf = p;
      job->size = size;
    }

    avail = job->size - job->len;
    job->emitted = emit(&job->ds, job->buf + job->len, &avail);
    job->len = job->size - avail;
    if (job->emitted != MORE)
      break;
  }

  job->crc = job->ds.crc;
  decoder_free(&job->ds);
  job->live = false;
}

static void
release(struct dec_job *job)
{
  if (job->live)
    decoder_free(&job->ds);
  job->live = false;
}

/* Uses the result of a task that started at the block whose header parse()
   has just read. */
static int
take(struct dec_job *job, const struct header *hd, uint8_t *out, size_t cap,
     size_t *produced)
{
  uint32_t crc;
  int rv;

  if (job->retrieved != OK ||
      job->block_size > (unsigned)hd->bs100k * 100000u)
    return LBZIP2_ERR_DATA;
  if (job->status != LBZIP2_OK)
    return job->status;
  if (job->emitted != OK && job->emitted != MORE)
    return LBZIP2_ERR_DATA;
  if (job->len > cap - *produced)
    return LBZIP2_ERR_OUTPUT;

  if (job->len > 0)
    memcpy(out + *produced, job->buf, job->len);
  *produced += job->len;
  crc = job->crc;
  if (job->emitted == MORE) {
    rv = emit_rest(&job->ds, out, cap, produced);
    if (rv != LBZIP2_OK)
      return rv;
    crc = job->ds.crc;
  }
  return crc == hd->crc ? LBZIP2_OK : LBZIP2_ERR_DATA;
}

static int
expand_parallel(struct parser_state *ps, struct bitstream *bs,
                const uint32_t *words, size_t nwords, size_t payload_size,
                uint8_t *out, size_t cap, size_t *produced,
                unsigned threads, const struct lbzip2_executor *executor)
{
  const size_t nslots = window_size(threads, executor);
  struct dec_job *jobs;
  uint64_t *starts;
  size_t nstarts, next, head, count, i;
  int rv;

  rv = find_blocks(words, nwords, threads, executor, &starts, &nstarts);
  if (rv != LBZIP2_OK)
    return rv;
  jobs = calloc(nslots, sizeof *jobs);
  if (jobs == NULL) {
    free(starts);
    return LBZIP2_ERR_MEMORY;
  }

  next = 0;
  head = 0;
  count = 0;
  for (;;) {
    struct header hd;
    unsigned garbage;
    uint64_t here;
    int prv;

    prv = parse(ps, &hd, bs, &garbage);
    if (prv == FINISH) {
      rv = check_end(bs, words, garbage, payload_size);
      break;
    }
    if (prv != OK) {
      rv = LBZIP2_ERR_DATA;
      break;
    }
    here = bits_pos(bs, words);

    /* Drop what started before this block: chance magic numbers. */
    while (count > 0 && jobs[head].start < here) {
      finish(executor, jobs[head].handle);
      release(&jobs[head]);
      head = (head + 1) % nslots;
      count--;
    }
    while (next < nstarts && starts[next] < here)
      next++;
    while (count < nslots && next < nstarts) {
      struct dec_job *job = &jobs[(head + count) % nslots];

      job->words = words;
      job->nwords = nwords;
      job->start = starts[next++];
      job->handle = start(executor, dec_task, job);
      count++;
    }

    if (count > 0 && jobs[head].start == here) {
      struct dec_job *job = &jobs[head];

      finish(executor, job->handle);
      head = (head + 1) % nslots;
      count--;
      rv = take(job, &hd, out, cap, produced);
      release(job);
      if (rv != LBZIP2_OK)
        break;
      *bs = bits_at(words, nwords, job->end);
    }
    else {
      rv = expand_here(&hd, bs, out, cap, produced);
      if (rv != LBZIP2_OK)
        break;
    }
  }

  while (count > 0) {
    finish(executor, jobs[head].handle);
    release(&jobs[head]);
    head = (head + 1) % nslots;
    count--;
  }
  for (i = 0; i < nslots; i++)
    free(jobs[i].buf);
  free(jobs);
  free(starts);
  return rv;
}

static int
expand_sequential(struct parser_state *ps, struct bitstream *bs,
                  const uint32_t *words, size_t payload_size,
                  uint8_t *out, size_t cap, size_t *produced)
{
  for (;;) {
    struct header hd;
    unsigned garbage;
    int rv;

    rv = parse(ps, &hd, bs, &garbage);
    if (rv == FINISH)
      return check_end(bs, words, garbage, payload_size);
    if (rv != OK)
      return LBZIP2_ERR_DATA;
    rv = expand_here(&hd, bs, out, cap, produced);
    if (rv != LBZIP2_OK)
      return rv;
  }
}

int
lbzip2_expand_buffer(const void *in, size_t in_size,
                     void *out, size_t *out_size,
                     unsigned threads,
                     const struct lbzip2_executor *executor)
{
  const uint8_t *ip = in;
  struct parser_state ps;
  struct bitstream bs;
  uint32_t *words;
  size_t nwords, payload_size, produced;
  int rv;

  if (out_size == NULL || (in == NULL && in_size > 0) ||
      (out == NULL && *out_size > 0))
    return LBZIP2_ERR_PARAM;

  /* The stream header gives the block size, which the parser needs before
     it starts; expand.c is given it the same way. */
  if (in_size < HEADER_SIZE || ip[0] != 0x42 || ip[1] != 0x5A ||
      ip[2] != 0x68 || ip[3] < 0x31 || ip[3] > 0x39)
    return LBZIP2_ERR_DATA;

  /* The decoder reads aligned 32-bit words in network byte order, so copy
     the rest of the input into a buffer padded with zeros to a whole word.
     That copy is cheap next to decoding it. */
  payload_size = in_size - HEADER_SIZE;
  nwords = (payload_size + 3) / 4;
  words = calloc(nwords > 0 ? nwords : 1, sizeof *words);
  if (words == NULL)
    return LBZIP2_ERR_MEMORY;
  memcpy(words, ip + HEADER_SIZE, payload_size);

  bs = bits_at(words, nwords, 0);
  parser_init(&ps, ip[3] - 0x30, 0);
  produced = 0;

  if (window_size(threads, executor) > 1)
    rv = expand_parallel(&ps, &bs, words, nwords, payload_size, out,
                         *out_size, &produced, threads, executor);
  else
    rv = expand_sequential(&ps, &bs, words, payload_size, out, *out_size,
                           &produced);

  free(words);
  if (rv != LBZIP2_OK)
    return rv;
  *out_size = produced;
  return LBZIP2_OK;
}
