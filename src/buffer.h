/*-
  buffer.h -- buffer-to-buffer compression and decompression

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
  An interface for programs that embed lbzip2's codec instead of running the
  lbzip2 tool, such as benchmarks: whole buffers in, whole buffers out,
  optionally spread over threads.

  It needs buffer.c and the codec, crctab.c decode.c divbwt.c encode.c and
  parse.c, but nothing of the tool.  The one thing the codec takes from the
  tool is xmalloc(), which decode.c calls and expects never to return NULL;
  main.c defines it for the tool, and a program that links the codec without
  main.c defines it too, for example as malloc() that aborts on failure.

  Threads come from an executor, which the caller provides, so the interface
  works with whatever threads the calling program has.  buffer_pthread.c is
  an executor built on POSIX threads.  The output does not depend on the
  executor or on the number of threads.
*/

#ifndef LBZIP2_BUFFER_H
#define LBZIP2_BUFFER_H

#include <stddef.h>             /* size_t */

#ifdef __cplusplus
extern "C" {
#endif


/* Return codes. */
#define LBZIP2_OK           0
#define LBZIP2_ERR_PARAM   -1   /* invalid argument */
#define LBZIP2_ERR_MEMORY  -2   /* out of memory */
#define LBZIP2_ERR_OUTPUT  -3   /* output buffer too small */
#define LBZIP2_ERR_DATA    -4   /* input is not bzip2 data, or is corrupt */

/* Flags for lbzip2_compress_buffer(). */
#define LBZIP2_SEQUENTIAL   1   /* fill blocks across the whole input, as -u */


/*
  Runs tasks on other threads.

  submit() starts fn(arg) and returns a handle for it, or returns NULL to have
  the caller run fn(arg) itself, for example when no thread is free or memory
  ran out.  wait() returns once the task with the given handle has finished,
  and releases the handle; it is called exactly once for every handle.  Tasks
  are independent of each other and never wait for anything, so an executor
  can run them in any order and on any number of threads, including none.
  Calls to the buffer functions below may share an executor; submit() and
  wait() are only ever called by the thread that called the buffer function.
*/
struct lbzip2_executor {
  void *(*submit)(void *opaque, void (*fn)(void *arg), void *arg);
  void (*wait)(void *opaque, void *handle);
  void *opaque;
};


/*
  Compresses in_size bytes at in into a single bzip2 stream at out, with
  blocks of level * 100k (level 1 to 9).  *out_size is the size of the output
  buffer on entry and the size of the stream on return.

  As the lbzip2 tool does by default, the input is taken in pieces of
  level * 100k bytes, and every piece starts a new block: blocks hold that
  much input after run-length coding, which shortens runs, so most are not
  full.  With LBZIP2_SEQUENTIAL in flags, as with -u, every block is filled
  before the next is started, which compresses runs a little better.

  With an executor and threads > 1, blocks are compressed as tasks on the
  executor, up to 2 * threads at a time; threads should be the number of
  threads the executor runs tasks on.  Without an executor, or with
  threads <= 1, everything runs on the calling thread.  The stream is the same
  either way, and the same as the lbzip2 tool writes with the same options.
*/
int lbzip2_compress_buffer(const void *in, size_t in_size,
                           void *out, size_t *out_size,
                           int level, int flags, unsigned threads,
                           const struct lbzip2_executor *executor);

/*
  Decompresses the bzip2 data at in, one or more concatenated streams, into
  out.  *out_size is the size of the output buffer on entry and the number of
  bytes decompressed on return.  Data after the last stream that is not
  another stream is ignored, as the lbzip2 tool ignores it.

  With an executor and threads > 1, the input is first searched for block
  headers, as the lbzip2 tool does, in tasks of a megabyte each; then the
  blocks are decompressed as tasks, up to 2 * threads at a time, while the
  calling thread follows the stream from one block to the next and takes
  each block's result in order.  Data that only looks like a block header is
  recognised as such, so what is accepted and what is rejected does not
  depend on the executor or the number of threads.  A block that
  decompresses to more than twice its size, which takes long runs such as of
  zeros, is finished by the calling thread, so that the blocks in flight hold
  a bounded amount of memory.
*/
int lbzip2_expand_buffer(const void *in, size_t in_size,
                         void *out, size_t *out_size,
                         unsigned threads,
                         const struct lbzip2_executor *executor);


/*
  An executor that runs tasks on a fixed set of POSIX threads, from
  buffer_pthread.c.  Returns NULL if threads is 0 or the threads cannot be
  started.  Destroy it only when no buffer function is using it.
*/
struct lbzip2_executor *lbzip2_pthread_executor_create(unsigned threads);
void lbzip2_pthread_executor_destroy(struct lbzip2_executor *executor);


#ifdef __cplusplus
}
#endif

#endif /* LBZIP2_BUFFER_H */
