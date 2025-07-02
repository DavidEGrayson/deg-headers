// arena.h v1.0.0
// Public domain arena, string, and container utilities for C/C++
// https://github.com/DavidEGrayson/deg-headers
//
// This header implements an arena: a linked list of large blocks of memory
// allocated from the system, from which you can allocate sub-sections to
// hold your own data.  Individual allocations are very efficient because
// they typically just involve a few simple checks and bumping a pointer.
// Instead of freeing each allocation individually, you free them all
// together when you are done using every one of them.
// This can make your code more efficient and less error-prone than the
// traditional plan of calling `malloc` and `free` for each individual
// allocation.
//
// The typical way to use this arena is:
// 1. Create an Arena struct and zero-initialize it.
// 2. Call `arena_alloc` or `arena_alloc_no_init` to allocate memory regions
//    from it, either directly or through a wrapper function like
//    `arena_alloc1`, or through the container types defined in this header.
// 3. Optionally call `arena_clear(&arena)` if you are done using the data
//    currently stored in the arena but plan to make more allocations later.
//    The largest block of memory in the arena is retained for future use.
// 4. Call `arena_free(&arena)` when you are done using the arena or just want
//    it to free all the memory it allocated, allowing the arena's memory
//    to be used for other purposes.
//
// To avoid complexity and reduce the risk of use-after-free bugs, you should
// minimize the number of arenas you have and the number of times you call
// `arena_clear` or `arena_free`.  One idea is to have a single arena where you
// allocate everything you need for a complex computation, and then you call
// `arena_clear` or `arena_free` when you are done using the result of the
// computation or have copied it to more permanent memory.  If the computation
// has multiple, distinct phases, you could reduce memory usage by having a
// second arena for short-lived objects that is cleared after each phase.
//
// This header also provides code that makes it easy to work with
// an arena-allocated null-terminated string (AString),
// an arena-allocated list of arbitrary itels (AList),
// and arena-allocated hash maps (AHash).
//
// Note: If compiling for C, you must use a modern compiler (GCC 13+) that
// supports C23, since this code uses enums with a specified type and
// typeof_unqual.
//
// This documentation continues in the comments below.

#include <assert.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARENA_FIRST_BLOCK_SIZE
#define ARENA_FIRST_BLOCK_SIZE 4096
#endif

#ifndef ARENA_SMALL_STRING_SIZE
#define ARENA_SMALL_STRING_SIZE 16
#endif

#ifndef ARENA_SMALL_LIST_SIZE
#define ARENA_SMALL_LIST_SIZE 16
#endif

#define MAGIC_ASTR  0xa3bff2a73e545341  // "AST>" + 4 non-ASCII bytes
#define MAGIC_ALI   0xb4a888b43e494c41  // "ALI>" + 4 non-ASCII bytes
#define MAGIC_AHASH 0x89cdfacf3e414841  // "AHA>" + 4 non-ASCII bytes

#ifndef __cplusplus

// T** (const not allowed on the T, T*) is changed to 'void**'
#define _ARENA_PP(x) (_Generic((x), typeof_unqual(**(x))**: (void **)(x)))

// T* and const T* pass through.
#define _ARENA_T_PTR(x, T) (_Generic((x), T *: (x), const T *: (x)))

// Pointer to x after conversion to type T.
#define _ARENA_T_VAL(x, T) ((T[1]){(x)})

// T* and const T* pass through.  T gets converted to T*, but it has to be an
// exact match, unlike _ARENA_T_VAL.
#define _ARENA_T_PTR_OR_VAL(x, T) (_Generic((x), T *: (x), const T *: (x), T: (typeof(x)[1]){(x)}))

#endif

//// Core arena code ///////////////////////////////////////////////////////////

typedef void (*ArenaNoMemoryCallback)(void *, size_t size);

typedef struct ArenaBlockHeader
{
  struct ArenaBlockHeader * prev;

  // The size of this block, in bytes, including this header.
  size_t size;
} ArenaBlockHeader;

// We align to max_align_t in order to make size_estimate more accurate.
static const size_t arena_block_overhead = sizeof(ArenaBlockHeader) +
  (-sizeof(ArenaBlockHeader) % alignof(max_align_t));

typedef struct Arena
{
  ArenaBlockHeader * block;

  // The last memory region allocated from this block.  Used by arena_resize.
  uintptr_t block_last_allocation;

  // The beginning of the free space at the end of the current block.
  uintptr_t block_remainder;

  // The end of the current block.
  uintptr_t block_end;

  // An estimate of the size a single block would need to be in order to
  // hold all the allocations currently assigned by calls to arena_alloc.
  // For efficiency, this does NOT include allocations in the current block
  // (arena->block) until we are done making those allocations.
  // If the alignments of the allocations do not exceed alignof(max_align_t),
  // and addresses returned by malloc and arena_block_overhead are both
  // multiples of max_align_t, then I believe this is guaranteed to be
  // an over-estimate.  TODO: proof
  size_t size_estimate;

  // The highest remembered value of size_estimate.
  // The arena uses this to anticipate how much memory will be needed.
  // The user can change this value at any time to manage the arena's memory
  // usage.  For example, you might reduce it by 10% periodically to make sure
  // the arena's memory usage does not permanently stay high due to a
  // one-time event.
  size_t size_estimate_high;

  // Callback to use when malloc fails, before ending the program.
  ArenaNoMemoryCallback no_memory_callback;
  void * no_memory_callback_data;

  // A random number used by hash containers.  You can initialize this
  // directly, or leave it at zero and the library will initialize it using
  // rand() the first time it is needed.
  uint64_t hash_key;
} Arena;

// Returns the total amount of memory the Arena has allocated for from the
// system using malloc.  This includes unused space in the blocks.
static inline size_t arena_memory_size(const Arena * arena)
{
  size_t size = 0;
  ArenaBlockHeader * header = arena->block;
  while (header)
  {
    size += header->size;
    header = header->prev;
  }
  return size;
}

// Increases the given value until it has the given alignment.
static inline size_t arena_align(size_t v, size_t alignment)
{
  assert(((alignment - 1) & alignment) == 0);
  size_t r = v + (-v & (alignment - 1));
  assert(r >= v);
  return r;
}

static void arena_handle_no_memory(Arena *, size_t) __attribute__((noreturn));

// Handles cases where malloc returned NULL (or the amount of memory we wanted
// would not even fit in a size_t).
static void arena_handle_no_memory(Arena * arena, size_t size)
{
  if (arena->no_memory_callback != NULL)
  {
    arena->no_memory_callback(arena->no_memory_callback_data, size);
  }
  fprintf(stderr, "Error: Failed to allocate %zu bytes.\n", size);
  exit(1);
}

// private function
static void arena_done_with_block(Arena * arena)
{
  if (arena->block == NULL) { return; }

  if (arena->size_estimate == 0)
  {
    arena->size_estimate = arena_block_overhead;
  }

  // This is the actual size used for allocations in the current block.
  size_t block_size_used =
    arena->block_remainder - ((uintptr_t)arena->block + arena_block_overhead);

  arena->size_estimate =
    arena_align(arena->size_estimate, alignof(max_align_t)) + block_size_used;

  if (arena->size_estimate_high < arena->size_estimate)
  {
    arena->size_estimate_high = arena->size_estimate;
  }
}

// Allocates a new block with the specified number of bytes available for
// payload data (and stop allocating from the current block).  This function
// should probably not be used in most applications but it could be useful for
// testing of low-level code.
static void arena_start_new_block(Arena * arena, size_t payload_size)
{
  arena_done_with_block(arena);

  size_t block_size = arena_block_overhead + payload_size;

  ArenaBlockHeader * new_block = (ArenaBlockHeader *)malloc(block_size);
  if (new_block == NULL) { arena_handle_no_memory(arena, block_size); }

  *new_block = (ArenaBlockHeader){ arena->block, block_size };
  arena->block = new_block;
  arena->block_last_allocation = 0;
  arena->block_remainder = (uintptr_t)arena->block + arena_block_overhead;
  arena->block_end = (uintptr_t)arena->block + arena->block->size;
}

// Ensures the arena has enough space available in its current block to handle
// an allocation of the given size and alignment.  Also returns the maximum
// allocation size it could handle with the given alignment.
static size_t arena_pre_alloc(Arena * arena, size_t size, size_t alignment)
{
  // (This is a duplicate of the check done in arena_alloc_no_init.)
  size_t abr = arena_align(arena->block_remainder, alignment);
  if (abr <= arena->block_end && arena->block_end - abr >= size)
  {
    // The block we already have is big enough.
    return arena->block_end - abr;
  }

  // Figure out the minimum block size we would need to allocate.
  size_t min_block_size = arena_align(arena_block_overhead, alignment) + size;

  size_t anticipation_size = 0;
  if (arena->block)
  {
    // Force the next block to be twice as large as the last block.
    // (Block sizes are always a power of 2.)
    anticipation_size = arena->block->size + 1;
  }
  else
  {
    // Make sure the first block is large enough to accomodate a 25% greater
    // demand than the highest demand we remember.
    anticipation_size = arena->size_estimate_high +
      (arena->size_estimate_high >> 2);
  }

  // Find the first power of 2 that is greater than or equal to
  // min_block_size, anticipation_size, and ARENA_FIRST_BLOCK_SIZE.
  if (min_block_size < anticipation_size)
  {
    min_block_size = anticipation_size;
  }
  size_t block_size = ARENA_FIRST_BLOCK_SIZE;
  while (block_size < min_block_size)
  {
    block_size <<= 1;
    if (block_size == 0) { arena_handle_no_memory(arena, SIZE_MAX); }
  }

  arena_start_new_block(arena, block_size - arena_block_overhead);

  abr = arena_align(arena->block_remainder, alignment);
  assert(abr <= arena->block_end && arena->block_end - abr >= size);
  return arena->block_end - abr;
}

// This is just like arena_alloc() except the memory is not initialized to
// zero, so it is less safe.
static void * arena_alloc_no_init(Arena * arena, size_t size, size_t alignment)
{
  size_t abr = arena_align(arena->block_remainder, alignment);
  if (abr > arena->block_end || arena->block_end - abr < size)
  {
    arena_pre_alloc(arena, size, alignment);
    abr = arena_align(arena->block_remainder, alignment);
  }
  arena->block_last_allocation = abr;
  arena->block_remainder = abr + size;
  return (void *)abr;
}

// Allocates memory from the arena with the specified size and alignment.
// The memory is initialized to zero.
//
// Note: If size is 0, this function could return a NULL pointer or a pointer
// equal to a previous allocation of size 0.
static inline void * arena_alloc(Arena * arena, size_t size, size_t alignment)
{
  void * allocation = arena_alloc_no_init(arena, size, alignment);
  memset(allocation, 0, size);
  return allocation;
}

// Attempts to resize a memory region that was previously allocated with
// arena_alloc, without moving it.  Returns true if successful.  It is OK to
// pass *any* pointer as the second argument to this function, but this function
// only works if that pointer points to the last allocation made from the arena,
// and there is enough room in the block.
//
// This does NOT zero-initialize any part of the allocated memory.
//
// Note: If you are trying to shrink a memory region and this function returns
// false, you are strongly encouraged to use the new smaller capacity anyway, to
// make the behavior of your program more predictable, and not so dependent on
// the order that arena operations were performed.
static bool arena_resize(Arena * arena, void * allocation, size_t new_size)
{
  if (arena->block == NULL) { return false; }
  uintptr_t a = (uintptr_t)allocation;
  if (a != arena->block_last_allocation) { return false; }
  assert(a <= arena->block_remainder);
  assert(a <= arena->block_end);
  if (arena->block_end - a < new_size) { return false; }
  arena->block_remainder = a + new_size;
  return true;
}

// private function
static void arena_free_block_list(ArenaBlockHeader * block)
{
  while (block)
  {
    ArenaBlockHeader * prev = block->prev;
    free(block);
    block = prev;
  }
}

// Free all blocks except the latest one, which the Arena will reuse.
static inline void arena_clear(Arena * arena)
{
  if (arena->block)
  {
    arena_done_with_block(arena);
    arena_free_block_list(arena->block->prev);
    arena->block->prev = NULL;
    arena->block_last_allocation = 0;
    arena->block_remainder = (uintptr_t)arena->block + arena_block_overhead;
    assert(arena->block_end == (uintptr_t)arena->block + arena->block->size);
  }
}

// Frees all the arena's blocks.
static inline void arena_free(Arena * arena)
{
  arena_done_with_block(arena);
  arena_free_block_list(arena->block);
  arena->block = NULL;
  arena->block_last_allocation = arena->block_remainder = arena->block_end = 0;
}

//// Individual helper functions ///////////////////////////////////////////////

// Macro that calls arena_alloc with the right arguments to allocate space for
// one object of the specified type.
#define arena_alloc1(arena, type) ((type *)arena_alloc(arena, sizeof(type), alignof(type)))

// Just like arena_printf but takes a va_list.
static char * arena_vprintf(Arena * arena, const char * format, va_list ap)
{
  // We make sure the block has enough space at least for a small string,
  // and then reserve the entire remainder of the block, but just temporarily.
  size_t capacity = arena_pre_alloc(arena, ARENA_SMALL_STRING_SIZE, 1);

  bool grew = false;
  while (1)
  {
    char * str = (char *)arena_alloc_no_init(arena, capacity, 1);
    va_list ap2;
    va_copy(ap2, ap);
    int result = vsnprintf(str, capacity, format, ap2);
    va_end(ap2);
    if (result < 0)
    {
      // This error probably never happens.  But if it does, we should give
      // the user some clue that it happened, so let's report it as a no memory
      // error.  It's not too far from the truth.
      arena_handle_no_memory(arena, 0xF0F0F002);
    }
    else if ((size_t)result < capacity)
    {
      // Success
      arena_resize(arena, str, result + 1);
      return str;
    }
    else if (grew)
    {
      // This shouldn't happen. We already grew the string once and there still
      // is not enough space.
      arena_handle_no_memory(arena, 0xF0F0F003);
    }

    // This block doesn't have enough space for the string.
    arena_resize(arena, str, 0);  // make arena stats more accurate

    capacity = result + 1;
    grew = true;
  }
}

// Stores the specified formatted string in the arena and returns a
// pointer to it.
static inline char * arena_printf(Arena * arena, const char * format, ...)
  __attribute__((format(printf,2,3)));
static inline char * arena_printf(Arena * arena, const char * format, ...)
{
  va_list ap;
  va_start(ap, format);
  char * str = arena_vprintf(arena, format, ap);
  va_end(ap);
  return str;
}

// Private function
static inline void _arena_invalidate_magic(uint64_t * m)
{
  *m = (*m & ~0xFF000000) | ('-' << 24);
}


////////////////////////////////////////////////////////////////////////////////
// AString: an expandable null-terminated string stored in an arena.
//
// The string can contain null bytes, and there is also a null byte after the
// contents.
//
// Actual object in memory:  AString header; char string[capacity + 1];
// C type:                   char *, pointing to string

typedef struct AString {
  Arena * arena;
  size_t length;    // not including null terminator
  size_t capacity;  // not including null terminator
  size_t magic;
} AString;

// Creates a new string object which has the specified capacity, which is the
// maximum length the string can have without resizing.
static char * astr_create(Arena * arena, size_t capacity)
{
  AString * astr = (AString *)arena_alloc_no_init(arena,
    sizeof(AString) + capacity + 1, alignof(AString));
  astr->arena = arena;
  astr->length = 0;
  astr->capacity = capacity;
  astr->magic = MAGIC_ASTR;
  char * str = (char *)astr + sizeof(AString);
  str[0] = 0;
  return str;
}

// private function
static inline AString * _astr_header(char * str)
{
  assert(str && (size_t)(((size_t *)str)[-1] == (size_t)MAGIC_ASTR));
  return (AString *)(str - sizeof(AString));
}

// Returns the length field from the AString's header, which should equal the
// number of characters it currently has, not counting the null terminator.
static inline size_t astr_length(const char * str)
{
  if (str == NULL) { return 0; }
  return _astr_header((char *)str)->length;
}

// Returns the capacity of the AString, which is the number of characters it
// could store (not counting the null terminator), without needing to resize.
static inline size_t astr_capacity(const char * str)
{
  return _astr_header((char *)str)->capacity;
}

// Creates a new AString that is a copy of the specified AString, with a
// capacity that is greater than or equal to the specified capacity.
static char * astr_copy(const char * old_str, size_t capacity)
{
  const AString * old_astr = _astr_header((char *)old_str);
  if (capacity < old_astr->length) { capacity = old_astr->length; }
  char * str = astr_create(old_astr->arena, capacity);
  AString * astr = _astr_header(str);
  astr->length = old_astr->length;
  memcpy(str, old_str, astr->length);
  str[astr->length] = 0;
  return str;
}

// Changes the capacity of the AString to be greater than or equal to the
// specified capacity, without changing its contents.
// Passing 0 is a good way to resize the string to the minimal size needed,
// returning all unneeded memory to the arena (which only works if
// you didn't allocate anything from that arena after the last time the
// string capacity changed).
static void astr_resize_capacity(char ** str, size_t new_capacity)
{
  AString * astr = _astr_header(*str);
  if (new_capacity < astr->length)
  {
    new_capacity = astr->length;
  }

  if (arena_resize(astr->arena, astr, sizeof(AString) + new_capacity + 1))
  {
    astr->capacity = new_capacity;
    return;
  }
  else if (new_capacity <= astr->capacity)
  {
    // The user asked to shrink the string but we can't actually give the space
    // back to the arena, so there is no point.
    return;
  }

  AString * old_astr = astr;
  char * old_str = *str;

  // Create a new string object that is a copy of the old one.
  *str = astr_copy(*str, new_capacity);

  // The old string object is not valid anymore: try to prevent its use.
  _arena_invalidate_magic(&old_astr->magic);
  old_str[0] = 0;
}

// Set the length of the AString, increasing the capacity if necessary.
// Any new characters added to the string are initialized to 0 (null).
static void astr_set_length(char ** str, size_t length)
{
  AString * astr = _astr_header(*str);
  if (length > astr->capacity) { astr_resize_capacity(str, length); }
  if (length > astr->length)
  {
    memset(*str + astr->length, 0, 1 + length - astr->length);
  }
  else
  {
    (*str)[length] = 0;
  }
  astr->length = length;
}

// Makes the AString be empty (length 0).
static inline void astr_clear(char ** str)
{
  astr_set_length(str, 0);
}

// Adds the specified (null-terminated) string to the end of the AString,
// growing the AString's capacity if necessary.
//
// Note: To avoid O(N^2) problems, when this function grows the string's
// capacity, it makes the capacity be double what is needed.  When you are
// done adding to the string, it is good to call `astr_resize_capacity(&str, 0)`
// to release the extra space back to the arena (which is only possible if you
// didn't allocate anything from that arena after the last time the string
// grew).
static inline void astr_puts(char ** str, const char * cstr)
{
  if (cstr == NULL) { cstr = "(null)"; }
  AString * astr = _astr_header(*str);
  assert(astr->capacity >= astr->length);
  size_t cstrlen = strlen(cstr);
  size_t new_length = astr->length + cstrlen;
  if (astr->capacity < new_length)
  {
    size_t new_capacity = new_length;
    if (new_capacity <= SIZE_MAX / 2) { new_capacity *= 2; }
    astr_resize_capacity(str, new_capacity);
    astr = _astr_header(*str);
  }
  memcpy(*str + astr->length, cstr, cstrlen + 1);
  astr->length = new_length;
  assert((*str)[astr->length] == 0);
}

// Just like astr_printf but takes a va_list.
static int astr_vprintf(char ** str, const char * format, va_list ap)
{
  bool grew = false;
  while (1)
  {
    AString * astr = _astr_header(*str);
    assert(astr->capacity >= astr->length);
    char * target = *str + astr->length;
    assert(*target == 0);
    size_t available = astr->capacity + 1 - astr->length;
    va_list ap2;
    va_copy(ap2, ap);
    int result = vsnprintf(target, available, format, ap2);
    va_end(ap2);
    if (result < 0)
    {
      // This error probably never happens.  But if it does, we should give
      // the user some clue that it happened, so let's report it as a no memory
      // error.  It's not too far from the truth.
      arena_handle_no_memory(astr->arena, 0xF0F0F000);
    }
    else if ((size_t)result < available)
    {
      // Success
      astr->length += result;
      return result;
    }
    else if (grew)
    {
      // This shouldn't happen. We already grew the string once and there still
      // is not enough space.
      arena_handle_no_memory(astr->arena, 0xF0F0F001);
    }

    // There wasn't enough capacity in the string, so the string needs to grow.
    *target = 0;  // Restore the string's contents.
    size_t new_capacity = astr->length + result;
    if (new_capacity <= SIZE_MAX / 2) { new_capacity *= 2; }
    astr_resize_capacity(str, new_capacity);
    grew = true;
  }
}

// Writes arbitrary binary data to an arbitrary offset in the string.
//
// Note: To avoid O(N^2) problems, when this function grows the string's
// capacity, it makes the capacity be double what is needed.  When you are
// done adding to the string, it is good to call `astr_resize_capacity(&str, 0)`
// to release the extra space back to the arena (which is only possible if you
// didn't allocate anything from that arena after the last time the string
// grew).
void astr_write_at_offset(char ** str, size_t offset, size_t size,
#ifdef __cplusplus
  const char data[]
#else
  const char data[size]
#endif
)
{
  AString * astr = _astr_header(*str);
  assert(astr->capacity >= astr->length);
  size_t required_length = offset + size;
  if (astr->capacity < required_length)
  {
    size_t new_capacity = required_length;
    if (new_capacity <= SIZE_MAX / 2) { new_capacity *= 2; }
    astr_resize_capacity(str, new_capacity);
    astr = _astr_header(*str);
  }
  if (astr->length < required_length)
  {
    if (astr->length < offset)
    {
      memset(*str + astr->length, 0, offset - astr->length);
    }
    (*str)[required_length] = 0;
    astr->length = required_length;
  }
  memcpy(*str + offset, data, size);
}

// Adds the specified formatted string to the end of the AString, growing
// the AString's capacity if necessary.
//
// Note: To avoid O(N^2) problems, when this function grows the string's
// capacity, it makes the capacity be double what is needed.  When you are
// done adding to the string, it is good to call `astr_resize_capacity(&str, 0)`
// to release the extra space back to the arena (which is only possible if you
// didn't allocate anything from that arena after the last time the string
// grew).
static inline int astr_printf(char ** str, const char * format, ...)
  __attribute__((format(printf,2,3)));
static inline int astr_printf(char ** str, const char * format, ...)
{
  va_list ap;
  va_start(ap, format);
  int r = astr_vprintf(str, format, ap);
  va_end(ap);
  return r;
}

// Just like astr_create_f but takes a va_list.
static char * astr_create_v(Arena * arena, const char * format, va_list ap)
{
  // We make sure the block has enough space at least for a small string,
  // and then reserve the entire remainder of the block, but just temporarily.
  size_t remainder = arena_pre_alloc(arena,
    sizeof(AString) + ARENA_SMALL_STRING_SIZE, alignof(AString));
  char * str = astr_create(arena, remainder - sizeof(AString));
  astr_vprintf(&str, format, ap);
  astr_resize_capacity(&str, 0);
  return str;
}

// Creates a new string containing the specified formatted string.
// The new string's capacity will be exactly large enough to hold the specified
// data; no other capacity is reserved.
static inline char * astr_create_f(Arena * arena, const char * format, ...)
  __attribute__((format(printf,2,3)));
static inline char * astr_create_f(Arena * arena, const char * format, ...)
{
  va_list ap;
  va_start(ap, format);
  char * str = astr_create_v(arena, format, ap);
  va_end(ap);
  return str;
}

// Destroys the AString and copies its contents to a regular C-style
// null-terminated string.  Returns a copy to the new string.
// Assuming that the AString was the last thing allocated in the arena, this
// saves memory in the Arena, but the copying is kind of wasteful and not ideal.
static inline char * astr_compact_into_cstr(char * str)
{
  AString * astr = _astr_header(str);
  _arena_invalidate_magic(&astr->magic);
  size_t size = astr->length + 1;
  if (arena_resize(astr->arena, astr, size))
  {
    // Assumption: The Arena is not multi-threaded, so nothing will overwrite
    // the data at the end of the string before we get a chance to copy it.
    char * new_cstr = (char *)astr;
    memmove(new_cstr, str, size);
    assert(new_cstr[size - 1] == 0);
    return new_cstr;
  }
  else
  {
    // We failed to resize the allocation, so don't waste time copying.
    return str;
  }
}

//// AList /////////////////////////////////////////////////////////////////////
// An AList (ali for short) is a resizable, null-terminated list of arbitrary
// C objects stored in an arena.
//
// An AList for objects of type T is represented by the C/C++ type "T *",
// which points to the beginning of the list.
// Since the list is null-terminated, it can be easily iterated by any code
// without needing to use this library.
//
// The memory layout of the AList is:
//   AList header;
//   T dropped[n];  (padding to ensure AList is aligned, if items were dropped)
//   T items[capacity + 1];
//
// Public interface for AList:
//
// T * ali_create(Arena *, size_t capacity, T);
//   Creates a list that has enough memory to hold the specified number of
//   objects ot type T, not counting the final null-terminator object.
//   Specify 0 as the capacity to use a reasonable default.
//   Note that ali_create is a macro, and T is actually a type,
//
// size_t ali_length(const T * list)
//   Gets the number of items stored in the AList (not counting the null
//   terminator).
//
// size_t ali_capacity(const T * list)
//   Gets the capacity of the AList, which is the number of items it could
//   store (not counting the null terminator) without needing to allocate more
//   memory.
//
// T* ali_copy(const T * list, size_t capacity)
//   Creates a new AList that is a copy of the specified AList, with a
//   capacity that is greater than or equal to the specified capacity.
//
// void ali_resize_capacity(T * & list, size_t capacity)
//   Changes the capacity of the AList without changing its contents.
//   If you specify a capacity less than the current list length, it will
//   automatically be increased to be equal to the list length.
//   Passing 0 is a good way to resize the list to the minimal size needed,
//   returning all unneeded memory to the arena (which only works if
//   you didn't allocate anything from that arena after the last time the
//   list capacity changed).
//
// void ali_set_length(T * & list, size_t length)
//   Set the length of the AList, increasing the capacity if necessary.
//   Any new items added to the list are initialized by setting them to zero.
//
// void ali_push(T * & list, T item)
//   Adds the specified item to the end of the AList, growing the list's
//   capacity if necessary.
//   To avoid O(N^2) problems, when this function grows the list, it makes
//   the capacity be double what is needed.
//
// void ali_drop(T * & list, size_t count)
//   Removes the first 'count' items from the list by changing the start of
//   the list.  Does not move any of the remaining items.
//
// Further explanations:
//
// In the documentation above, "T" is the type of item that the user wants
// to store in the list: T can be any C object, including a pointer or struct.
//
// "const T *" means the function takes an AList and does not modify
// the list or the objects it points to.
//
// "T * &" means the function takes a *reference* to an AList (implemented using
// pointers in C).  The function modifies the AList and if the list grows, then
// it might move to a different location, invalidating the old AList object and
// any other "T *" objects pointing to it.  The function uses the reference
// to update the "T *" object you pass to it.

typedef struct AList {
  Arena * arena;
  size_t length;    // number of items stored, not counting the NULL terminator
  size_t capacity;  // maximum length we can accomodate without resizing
  uint32_t item_size;
  size_t magic;
} AList;

static void * _ali_create(Arena * arena, size_t capacity, size_t item_size, size_t item_alignment)
{
  // When we allocate the block for the AList header and item array, we will
  // just align it using alignof(AList), then add sizeof(AList) to it, and
  // assume that is aligned enough for the item array.
  assert(alignof(AList) % item_alignment == 0);
  assert(sizeof(AList) % item_alignment == 0);
  assert(item_size % item_alignment == 0);

  if (capacity == 0) { capacity = ARENA_SMALL_LIST_SIZE; }
  size_t size = sizeof(AList) + (capacity + 1) * item_size;
  AList * ali = (AList *)arena_alloc_no_init(arena, size, alignof(AList));
  ali->arena = arena;
  ali->length = 0;
  ali->capacity = capacity;
  ali->item_size = item_size;
  ali->magic = MAGIC_ALI;
  void * list = (void *)((uint8_t *)ali + sizeof(AList));
  memset(list, 0, item_size);
  return list;
}

static inline AList * _ali_header_no_check(const void * list)
{
  // The & operator is needed for lists where sizeof(T) < alignof(AList)
  // and ali_drop has been used.
  return (AList *)(((uintptr_t)list - sizeof(AList)) & ~(alignof(AList) - 1));
}

static inline AList * _ali_header(const void * list)
{
  assert(list);
  AList * h = _ali_header_no_check(list);
  assert(h->magic == MAGIC_ALI);
  return h;
}

static inline size_t _ali_length(const void * list)
{
  if (list == NULL) { return 0; }
  return _ali_header(list)->length;
}

static inline size_t _ali_capacity(const void * list)
{
  return _ali_header(list)->capacity;
}

static void * _ali_copy(const void * old_list, size_t capacity)
{
  AList * old_h = _ali_header(old_list);

  if (capacity < old_h->length) { capacity = old_h->length; }

  void * list = _ali_create(old_h->arena, capacity, old_h->item_size, 1);
  AList * h = _ali_header(list);

  h->length = old_h->length;
  memcpy(list, old_list, (old_h->length + 1) * old_h->item_size);
  return list;
}

static void _ali_resize_capacity(void ** list, size_t new_capacity)
{
  assert(list);
  AList * h = _ali_header(*list);

  if (new_capacity < h->length) { new_capacity = h->length; }

  size_t size = sizeof(AList) + (new_capacity + 1) * sizeof(void *);
  if (arena_resize(h->arena, h, size))
  {
    h->capacity = new_capacity;
    return;
  }
  else if (new_capacity <= h->capacity)
  {
    // The user asked to shrink the capacity of the list, but we cannot give
    // that memory back to the arena, so there is no point.
    return;
  }

  AList * old_h = h;
  void * old_list = *list;

  *list = _ali_copy(old_list, new_capacity);

  // The old object is not valid anymore: try to prevent its use.
  _arena_invalidate_magic(&old_h->magic);
  old_h->length = 0;
  memset(old_h, 0, old_h->item_size);
}

static inline void _ali_set_length(void ** list, size_t length)
{
  assert(list);
  AList * h = _ali_header(*list);
  if (length > h->capacity) { _ali_resize_capacity(list, length); }
  if (length > h->length)
  {
    memset((uint8_t *)*list + h->length * h->item_size, 0, (length - h->length) * h->item_size);
  }
  h->length = length;
  memset((uint8_t *)*list + h->length * h->item_size, 0, h->item_size);
}

static inline void * _ali_push0(void ** list)
{
  assert(list);
  AList * h = _ali_header(*list);
  if (h->length >= h->capacity)
  {
    size_t new_capacity = h->length + 1;
    if (new_capacity <= SIZE_MAX / 2) { new_capacity *= 2; }
    _ali_resize_capacity(list, new_capacity);
    h = _ali_header(*list);
  }
  h->length++;
  memset((uint8_t *)*list + h->length * h->item_size, 0, h->item_size);
  return (uint8_t *)*list + (h->length - 1) * h->item_size;
}

static inline void _ali_drop(void ** list, size_t count)
{
  assert(list);
  AList * h = _ali_header(*list);
  if (count > h->length) { count = h->length; }
  void * new_list = (uint8_t *)*list + count * h->item_size;
  *list = new_list;
  AList * new_h = _ali_header_no_check(new_list);
  memmove(new_h, h, sizeof(AList));
  new_h->capacity -= count;
  new_h->length -= count;
}

#define ali_create(arena, capacity, T) ((T *)_ali_create((arena), (capacity), sizeof(T), alignof(T)))
#define ali_length _ali_length
#define ali_capacity _ali_capacity

#ifdef __cplusplus

template <typename T> static inline T * ali_copy(const T * list, size_t capacity)
{
  return (T *)_ali_copy(list, capacity);
}

template<typename T> static inline void ali_resize_capacity(T * & list, size_t capacity)
{
  _ali_resize_capacity((void **)&list, capacity);
}

template<typename T> static inline void ali_set_length(T * & list, size_t length)
{
  _ali_set_length((void **)&list, length);
}

template<typename T, typename U> static inline void ali_push(T * & list, U item)
{
  *(T *)_ali_push0((void **)&list) = item;
}

template<typename T> static inline void ali_drop(T * & list, size_t count)
{
  _ali_drop((void **)&list, count);
}

#else
#define ali_copy(list, cap) ((typeof_unqual(*list)*)_ali_copy((list), cap))
#define ali_resize_capacity(list, cap) (_ali_resize_capacity(_ARENA_PP(&list), cap))
#define ali_set_length(list, length) (_ali_set_length(_ARENA_PP(&list), length))
#define ali_push(list, item) (*(typeof(list))_ali_push0(_ARENA_PP(&list)) = (item))
#define ali_drop(list, count) (_ali_drop(_ARENA_PP(&list), (count)))
#endif

///// Hash function ////////////////////////////////////////////////////////////
// This code was originally copied from
// https://github.com/veorq/SipHash/blob/8e6e4c1/halfsiphash.c
// It has been modified.

/*
   SipHash reference C implementation

   Copyright (c) 2016 Jean-Philippe Aumasson <jeanphilippe.aumasson@gmail.com>

   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along
   with
   this software. If not, see
   <http://creativecommons.org/publicdomain/zero/1.0/>.
 */

/* default: SipHash-2-4 */
#define cROUNDS 2
#define dROUNDS 4

#define ROTL(x, b) (uint32_t)(((x) << (b)) | ((x) >> (32 - (b))))

#define U32TO8_LE(p, v)                                                        \
    (p)[0] = (uint8_t)((v));                                                   \
    (p)[1] = (uint8_t)((v) >> 8);                                              \
    (p)[2] = (uint8_t)((v) >> 16);                                             \
    (p)[3] = (uint8_t)((v) >> 24);

#define U32TO8_LE(p, v)                                                        \
    (p)[0] = (uint8_t)((v));                                                   \
    (p)[1] = (uint8_t)((v) >> 8);                                              \
    (p)[2] = (uint8_t)((v) >> 16);                                             \
    (p)[3] = (uint8_t)((v) >> 24);

#define U8TO32_LE(p)                                                           \
    (((uint32_t)((p)[0])) | ((uint32_t)((p)[1]) << 8) |                        \
     ((uint32_t)((p)[2]) << 16) | ((uint32_t)((p)[3]) << 24))

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = ROTL(v1, 5);                                                      \
        v1 ^= v0;                                                              \
        v0 = ROTL(v0, 16);                                                     \
        v2 += v3;                                                              \
        v3 = ROTL(v3, 8);                                                      \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = ROTL(v3, 7);                                                      \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = ROTL(v1, 13);                                                     \
        v1 ^= v2;                                                              \
        v2 = ROTL(v2, 16);                                                     \
    } while (0)

static int arena_halfsiphash(const uint8_t *in, const size_t inlen, const uint8_t *k,
                uint8_t *out, const size_t outlen) {

    assert((outlen == 4) || (outlen == 8));
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = 0x6c796765;
    uint32_t v3 = 0x74656462;
    uint32_t k0 = U8TO32_LE(k);
    uint32_t k1 = U8TO32_LE(k + 4);
    uint32_t m;
    int i;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint32_t));
    const int left = inlen & 3;
    uint32_t b = ((uint32_t)inlen) << 24;
    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    if (outlen == 8)
        v1 ^= 0xee;

    for (; in != end; in += 4) {
        m = U8TO32_LE(in);
        v3 ^= m;

        for (i = 0; i < cROUNDS; ++i)
            SIPROUND;

        v0 ^= m;
    }

    switch (left) {
    case 3:
        b |= ((uint32_t)in[2]) << 16;  /* fall-through */
    case 2:
        b |= ((uint32_t)in[1]) << 8;   /* fall-through */
    case 1:
        b |= ((uint32_t)in[0]);
        break;
    case 0:
        break;
    }

    v3 ^= b;

    for (i = 0; i < cROUNDS; ++i)
        SIPROUND;

    v0 ^= b;

    if (outlen == 8)
        v2 ^= 0xee;
    else
        v2 ^= 0xff;

    for (i = 0; i < dROUNDS; ++i)
        SIPROUND;

    b = v1 ^ v3;
    U32TO8_LE(out, b);

    if (outlen == 4)
        return 0;

    v1 ^= 0xdd;

    for (i = 0; i < dROUNDS; ++i)
        SIPROUND;

    b = v1 ^ v3;
    U32TO8_LE(out + 4, b);

    return 0;
}

#undef cROUNDS
#undef dROUNDS
#undef ROTL
#undef U32TO8_LE
#undef U32TO8_LE
#undef U8TO32_LE
#undef SIPROUND

static inline uint64_t arena_hash_random_key()
{
  uint64_t key = 0;
  while (key == 0)
  {
    for (uint8_t i = 0; i < 8; i++)
    {
      key = (key << 8) | (rand() & 0xFF);
    }
  }
  return key;
}

typedef uint32_t ArenaHashInt;

// Initializes the arena's hash key, if it is not already initialized.
// You do not need to call this directly: the key is automatically
// initialized when needed.
static void arena_hash_key_init(Arena * arena)
{
  while (arena->hash_key == 0)
  {
    for (size_t i = 0; i < 8; i++)
    {
      arena->hash_key = (arena->hash_key << 8) | (rand() & 0xFF);
    }
  }
}

// Calculates the hash of the specified data.
// Never returns 0 (the empty value) or 1 (the tombstone value).
static ArenaHashInt arena_hash(Arena * arena,
  const uint8_t * data, size_t length)
{
  arena_hash_key_init(arena);
  ArenaHashInt out;
  arena_halfsiphash(data, length, (uint8_t *)&arena->hash_key,
    (uint8_t *)&out, sizeof(out));
  if (out <= 1) { out = 2; }
  return out;
}

// Calculates the hash of the specified string.  Never returns 0.
static ArenaHashInt arena_hash_from_string(Arena * arena,
  const char * str)
{
  return arena_hash(arena, (const uint8_t *)str, strlen(str));
}

//// AByteSlice ////////////////////////////////////////////////////////////////

// This struct just represents an arbitrary piece of binary data somewhere.
typedef struct AByteSlice {
  uint8_t * data;
  size_t size;
} AByteSlice;

//// AHash /////////////////////////////////////////////////////////////////////
// An AHash is a resizable, null-terminated array of items stored in an arena
// that has a hash table associated with it for fast lookups of items.
//
// The type of the items is called "T", and it must be a struct whose first
// member is named "key", whose type we call TK.  An AHash holding items of
// type T is represented by a C/C++ variable of type "T *" that points to the
// start of the array.
//
// Since the array is null-terminated (meaning the last item is all zeros),
// it can be easily iterated by any code without needing to use this library
// (but the keys or the order of items or doing fast lookups will require this
// library).
//
// The memory layout of the main object of the AHash is:
//   AHash header;
//   T items[capacity + 1];
//
// One of the members in the header points to a table used to find items:
//   ArenaHashInt hash_table[capacity * 4];
//
// There are capacity * 2 slots in the hash table.  When we add an item to the
// table, the index of the slot we use for it is determined in part by the
// lower bits of the hash of its key, making it possible to quickly find
// the slot later.  The first half of the hash table stores the full hash
// of the item's key, or 0 if the slot is empty or 1 if the slot holds a
// deleted item.  The second half of the hash table stores the index of
// the item in the array.
//
// The capacity is always a power of 2.
//
// ArenaHashInt is uint32_t, which limits the size of the number of items to
// 2,147,483,648.  If that is a problem, you should be able to simply change
// the definition of ArenaHashInt to uint64_t.
//
// Supported key types:
//
// - AKEY_DEFAULT (0):
//   Each key is just treated as an opaque piece of fixed-size data.  Items are
//   hashed and compared based on the data directly contained in the key.
//
// - AKEY_STRING:
//   The key is treated as a 'const char *', and it should point to a
//   null-terminated string that won't change while it is in use by the hash.
//   Items are hashed and compared based on the contents of the strings.
//
// - AKEY_BYTE_SLICE:
//   The key should have type AByteSlice (or a struct that is the same) and
//   it should point to some arbitrary data that won't change while it is in
//   use by the hash.  Items are hashed and compared based on the contents of
//   the data.
//
// Public interface for AHash:
//
// T * ahash_create(Arena *, size_t capacity, AKeyType type, T);
//   Creates a hash that has enough memory to hold the specified number of
//   items of type T.  Note that this is a macro and the last argument is
//   just a type, not a value.  This function doesn't auto-detect the
//   key type because if the key is a 'const char *', we don't know if it
//   points to arbitrary strings (so the type should be AKEY_STRING) or
//   interned strings that are equal if and only if their addresses are equal
//   (so the key type should be AKEY_DEFAULT).  Note that the address of
//   hash, which is returned by this function, can change when the hash grows.
//
// size_t ahash_length(const T * hash)
//   Returns the number of items stored in the hash.
//
// size_t ahash_capacity(const T * hash)
//   Returns the number of items (including deleted items) the hash table can
//   store without needing to grow or rebuild anything.
//   (The hash table internally contains capacity * 2 slots used to look up
//   an item quickly based on the hash of a key.)
//
// T * ahash_copy(const T * hash, size_t capacity)
//   Creates a new AHash that is a copy of the specified AHash, with a
//   capacity that is greater than or equal to the specified capacity.
//
// void ahash_resize_capacity(T * & hash, size_t capacity)
//   This function ensures the hash table has the specified capacity or more.
//   Unlike astr_resize_capacity and ali_resize_capacity, this function
//   never decreases the capacity of the hash table, because there is no
//   practical way to give that space back to the arena.
//   Because deleted items take up slots in the hash table, calling
//   this function is not sufficient to ensure that the hash table is
//   ready to accept any particular number of new items being added to it.
//   See ahash_ensure_space if that is what you are trying to do.
//
// void ahash_ensure_space(T * & hash, size_t count)
//   Ensures that the hash can accomodate 'count' additional items being added
//   to it without needing to allocate more memory or rebuild anything.
//
// T * ahash_find(const T * hash, TK key);
// T * ahash_find_p(const T * hash, const TK * key);
//   Looks for an item with the specified key.  Returns a
//   pointer to it if it exists, or NULL if it does not exist
//   (the item's location is not permanent: it can move when the table grows)
//   If you're curious why the ahash_find have to be separate ahash_find_p macros, see:
//   https://gist.github.com/DavidEGrayson/44a54453af0ea0ec890c615b81dbbd0c
//
// T * ahash_update(T * & hash, const T * item);
// T * ahash_update(T * & hash, T item);
//   Copies the specified item into the hash table and returns a pointer to its
//   new location (which is not permanent: it can move when the table
//   grows).  If an item already existed in the hash table with the same key,
//   this function overwrites it completely.
//
// T * ahash_find_or_update(T * & hash, const T * item, bool * found);
// T * ahash_find_or_update(T * & hash, T item, bool * found);
//   Looks for an item with the specified key.  If it is found, then
//   this function sets *found to true.  If it is not found, this function
//   sets *found to false and copies all the data from 'item' into the hash
//   table.  Returns a pointer to the location of the item that was found or
//   added (which is not permanent: it can move when the table grows).
//
// bool ahash_delete(T * & hash, TK key)
// bool ahash_delete_p(T * & hash, TK * key)
//   Deletes the item with the specified key, removing it from the array and
//   moving the last item in the array to take its place.  Returns true if an
//   item was deleted.

typedef enum AKeyType : uint8_t {
  AKEY_DEFAULT = 0,
  AKEY_STRING = 1,
  AKEY_BYTE_SLICE = 2,
} AKeyType;

static const size_t ahash_max_capacity = (ArenaHashInt)-1 / 2 + 1;

typedef struct AHash {
  Arena * arena;
  ArenaHashInt * table;
  ArenaHashInt * spare_table;  // if present, is the same size as 'table'
  ArenaHashInt length;    // number of items stored, not counting the NULL terminator
  ArenaHashInt capacity;  // maximum length we can accomodate without resizing (power of 2)
  ArenaHashInt tombstone_count;  // number of hash slots used by deleted items
  uint32_t item_size;
  uint32_t key_size;
  AKeyType key_type;
  size_t magic;
} AHash;

// Calculate the actual capacity to use for an AHash.  It will be at least
// as large as the reuqested capacity, but if the requested capcaity is too
// large then this function does not return and triggers the no memory
// handler.
static size_t _ahash_calculate_capacity(Arena * arena, size_t requested)
{
  if (requested == 0) { requested = ARENA_SMALL_LIST_SIZE; }
  size_t capacity = 1;
  while (capacity < requested)
  {
    if (capacity >= ahash_max_capacity)
    {
      // Our hash function doesn't return enough bits to handle the requested
      // capacity.
      arena_handle_no_memory(arena, 0xF0F0F004);
    }
    capacity <<= 1;
  }
  return capacity;
}

// Calculate the number of bytes needed for the main portion of an AHash.
static inline size_t _ahash_main_size(size_t capacity, size_t item_size)
{
  return sizeof(AHash) + (capacity + 1) * item_size;
}

// Calculates the number of bytes needed for the hash table portion of an AHash.
static inline size_t _ahash_table_size(size_t capacity)
{
  return (capacity * 4) * sizeof(ArenaHashInt);
}

static inline void * _ahash_create(Arena * arena, size_t capacity, AKeyType type,
  size_t key_size, size_t item_size, size_t item_alignment)
{
  // When we allocate the block for the AHash header and item array, we will
  // just align it using alignof(AHash), then add sizeof(Hash) to it, and
  // assume that is aligned enough for the item array.
  assert(alignof(AHash) % item_alignment == 0);
  assert(sizeof(AHash) % item_alignment == 0);
  assert(item_size % item_alignment == 0);

  assert(key_size <= item_size);
  switch(type)
  {
  case AKEY_STRING: assert(key_size == sizeof(char *)); break;
  case AKEY_BYTE_SLICE: assert(key_size == sizeof(char *) * 2); break;
  default: assert(key_size); break;
  }

  capacity = _ahash_calculate_capacity(arena, capacity);

  AHash * ahash = (AHash *)arena_alloc_no_init(arena,
    _ahash_main_size(capacity, item_size), alignof(AHash));
  memset(ahash, 0, sizeof(AHash));

  assert(alignof(AHash) % alignof(ArenaHashInt) == 0);
  ahash->table = (ArenaHashInt *)arena_alloc(arena,
    _ahash_table_size(capacity), alignof(AHash));

  ahash->arena = arena;
  ahash->length = 0;
  ahash->capacity = capacity;
  ahash->item_size = item_size;
  assert(item_size == ahash->item_size);
  ahash->key_type = type;
  ahash->key_size = key_size;
  ahash->magic = MAGIC_AHASH;
  void * list = ahash + 1;
  memset(list, 0, item_size);
  return list;
}

static inline AHash * _ahash_header(const void * hash)
{
  assert(((size_t *)hash)[-1] == (size_t)MAGIC_AHASH);
  assert(hash && ((size_t *)hash)[-1] == (size_t)MAGIC_AHASH);
  return (AHash *)((uint8_t *)hash - sizeof(AHash));
}

static inline size_t _ahash_length(const void * hash)
{
  if (hash == NULL) { return 0; }
  return _ahash_header(hash)->length;
}

static inline size_t _ahash_capacity(const void * hash)
{
  return _ahash_header(hash)->capacity;
}

static void * _ahash_copy(const void * old_hash, size_t capacity)
{
  const AHash * old_ahash = _ahash_header(old_hash);
  const ArenaHashInt * old_table = old_ahash->table;

  if (capacity < old_ahash->length) { capacity = old_ahash->length; }
  capacity = _ahash_calculate_capacity(old_ahash->arena, capacity);

  // Create the new header.
  AHash * ahash = (AHash *)arena_alloc_no_init(old_ahash->arena,
    _ahash_main_size(capacity, old_ahash->item_size), alignof(AHash));
  memset(ahash, 0, sizeof(AHash));
  ahash->arena = old_ahash->arena;
  ahash->length = old_ahash->length;
  ahash->capacity = capacity;
  ahash->item_size = old_ahash->item_size;
  ahash->key_type = old_ahash->key_type;
  ahash->key_size = old_ahash->key_size;
  ahash->magic = MAGIC_AHASH;

  // Copy the items and the null terminator.
  void * hash = (void *)((uint8_t *)ahash + sizeof(AHash));
  memcpy(hash, old_hash, (ahash->length + 1) * ahash->item_size);

  // Create the new table.
  assert(alignof(AHash) % alignof(ArenaHashInt) == 0);
  ArenaHashInt * table = ahash->table = (ArenaHashInt *)arena_alloc(
    old_ahash->arena, _ahash_table_size(capacity), alignof(AHash));
  for (size_t s = 0; s < old_ahash->capacity * 2; s++)
  {
    if (old_table[s] == 0) { continue; }  // skip empty slots
    const ArenaHashInt mask = capacity * 2 - 1;
    ArenaHashInt slot = old_table[s] & mask;
    while (table[slot]) { slot = (slot + 1) & mask; }
    table[slot] = old_table[s];
    table[2 * capacity + slot] = old_table[2 * old_ahash->capacity + s];
  }

  return hash;
}

static void _ahash_resize_capacity(void ** hash, size_t capacity)
{
  AHash * ahash = _ahash_header(*hash);
  if (capacity < ahash->length) { capacity = ahash->length; }
  capacity = _ahash_calculate_capacity(ahash->arena, capacity);
  if (capacity <= ahash->capacity)
  {
    // We have not implemented any way to return extra hash capacity to
    // the arena, so just ignore requests to shrink the hash.
    return;
  }

  // TODO: if possible, reuse memory from the old hash
  *hash = _ahash_copy(*hash, capacity);

  _arena_invalidate_magic(&ahash->magic);
}

// Applies the hash function to the key of the item.
static ArenaHashInt _ahash_calculate_hash(const void * hash, const void * key)
{
  AHash * ahash = _ahash_header(hash);
  switch (ahash->key_type)
  {
  case AKEY_STRING:
    return arena_hash_from_string(ahash->arena, *(const char **)key);
  case AKEY_BYTE_SLICE:
    {
      AByteSlice * bs = (AByteSlice *)key;
      return arena_hash(ahash->arena, bs->data, bs->size);
    }
  default:
    return arena_hash(ahash->arena, (uint8_t *)key, ahash->key_size);
  }
}

// Compares the keys of two items and returns true if they are equal.
static bool _ahash_compare(const void * hash, const void * key1, const void * key2)
{
  const AHash * ahash = _ahash_header(hash);
  switch (ahash->key_type)
  {
  case AKEY_STRING:
    return !strcmp(*(const char **)key1, *(const char **)key2);
  case AKEY_BYTE_SLICE:
    {
      AByteSlice * bs1 = (AByteSlice *)key1;
      AByteSlice * bs2 = (AByteSlice *)key2;
      return bs1->size == bs2->size && !memcmp(bs1->data, bs2->data, bs1->size);
    }
  default:
    return !memcmp(key1, key2, ahash->key_size);
  }
}

// Finds the slot in the hash table where an item with the specified key is being stored
// or could be stored.  The latter case is indicated by table[slot] == 0.
static inline ArenaHashInt _ahash_find_slot(const void * hash, const void * key)
{
  const AHash * ahash = _ahash_header(hash);
  size_t capacity = ahash->capacity;
  ArenaHashInt * table = ahash->table;
  ArenaHashInt hv = _ahash_calculate_hash(hash, key);
  ArenaHashInt mask = capacity * 2 - 1;
  ArenaHashInt slot = hv & mask;
  while (table[slot])
  {
    if (table[slot] == hv)
    {
      size_t found_index = table[capacity * 2 + slot];
      assert(found_index < ahash->length);
      void * found_item = (void *)((uint8_t *)hash + found_index * ahash->item_size);
      if (_ahash_compare(hash, key, found_item))
      {
        return slot;  // Found the item.  Return its slot.
      }
    }
    slot = (slot + 1) & mask;
  }
  return slot;  // Item not found.  Return an empty slot.
}

static inline void * _ahash_find(const void * hash, const void * key)
{
  const AHash * ahash = _ahash_header(hash);
  size_t capacity = ahash->capacity;
  ArenaHashInt * table = ahash->table;
  ArenaHashInt hv = _ahash_calculate_hash(hash, key);
  ArenaHashInt mask = capacity * 2 - 1;
  ArenaHashInt slot = hv & mask;
  while (table[slot])
  {
    if (table[slot] == hv)
    {
      size_t found_index = table[capacity * 2 + slot];
      assert(found_index < ahash->length);
      void * found_item = (void *)((uint8_t *)hash + found_index * ahash->item_size);
      if (_ahash_compare(hash, key, found_item))
      {
        return found_item;
      }
    }
    slot = (slot + 1) & mask;
  }
  return NULL;
}

// Removes all the tombstones from the hash table.
// TODO: we can do this without a second table, right?  Get rid of spare_table member.
static void _ahash_clean_table(void * hash)
{
  AHash * ahash = _ahash_header(hash);
  size_t capacity = ahash->capacity;
  if (ahash->spare_table == NULL)
  {
    ahash->spare_table = (ArenaHashInt *)arena_alloc_no_init(ahash->arena,
      _ahash_table_size(capacity), alignof(ArenaHashInt));
  }

  ArenaHashInt * old_table = ahash->table;
  ArenaHashInt * new_table = ahash->spare_table;
  memset(new_table, 0, capacity * 2 * sizeof(ArenaHashInt));
  ArenaHashInt mask = capacity * 2 - 1;
  for (size_t i = 0; i < capacity * 2; i++)
  {
    ArenaHashInt hv = old_table[i];
    if (hv) { return; }
    ArenaHashInt index = old_table[capacity * 2 + i];
    // hv points to index in the old table, so add that to the new one.
    ArenaHashInt slot = hv & mask;
    while (new_table[slot]) { slot = (slot + 1) & mask; }
    new_table[slot] = hv;
    new_table[capacity * 2 + slot] = index;
  }
  // TODO: maybe combine this with the code in resize_capacity that does something similar?

  ahash->table = new_table;
  ahash->spare_table = old_table;
  ahash->tombstone_count = 0;
}

static void _ahash_ensure_space(void ** hash, size_t count)
{
  AHash * ahash = _ahash_header(*hash);
  if (count <= ahash->capacity - ahash->tombstone_count - ahash->length)
  {
    return;  // We already have enough space.
  }

  if (count >= ahash_max_capacity - ahash->length)
  {
    // The hash cannot hold the requested amount of data.
    arena_handle_no_memory(ahash->arena, 0xF0F0F005);
  }

  size_t future_length = ahash->length + count;

  // desired_capacity = future_length would be bad: imagine a scenario where
  // capacity = 1024, length = 1023, tombstone_count = 1.  When the user adds
  // an item to the hash, this function is called and future_length would be
  // 1024, so we wouldn't resize the hash table, but we would spend O(N)
  // time cleaning up the tombstone, and then we'd have to do it again after the
  // deletes that item and tries to add anohter item.  It's terrible to do
  // O(N) work repeatedly every time the user does 2 operations.

  // desired_capacity = future_length * 2 would be wasteful: if there are no
  // tombstones, we should do the usual thing of doubling when needed.
  // So when future_length = 17, we want to get a capacity of 32, not 64.

  size_t desired_capacity = future_length + future_length / 2;
  if (desired_capacity > ahash_max_capacity)
  {
    desired_capacity = ahash_max_capacity;
  }

  _ahash_resize_capacity(hash, desired_capacity);

  ahash = _ahash_header(*hash);
  if (ahash->tombstone_count)
  {
    _ahash_clean_table(*hash);
  }
}

static inline void * _ahash_find_or_update(void ** hash, const void * item, bool * found)
{
  _ahash_ensure_space(hash, 1);

  AHash * ahash = _ahash_header(*hash);
  ArenaHashInt capacity = ahash->capacity;
  ArenaHashInt * table = ahash->table;
  ArenaHashInt hv = _ahash_calculate_hash(*hash, item);
  ArenaHashInt mask = capacity * 2 - 1;
  ArenaHashInt slot = hv & mask;
  while (table[slot])
  {
    if (table[slot] == hv)
    {
      size_t other_index = table[capacity * 2 + slot];
      assert(other_index < ahash->length);
      void * other_item = (void *)((uint8_t *)*hash + other_index * ahash->item_size);
      if (_ahash_compare(*hash, item, other_item))
      {
        // Found an existing item with the same key.
        *found = true;
        return other_item;
      }
    }
    slot = (slot + 1) & mask;
  }

  *found = false;
  size_t index = ahash->length++;
  ahash->table[slot] = hv;
  ahash->table[capacity * 2 + slot] = index;
  uint8_t * new_item = (uint8_t *)*hash + index * ahash->item_size;
  memcpy(new_item, item, ahash->item_size);
  memset(new_item + ahash->item_size, 0, ahash->item_size);
  return new_item;
}

static inline void * _ahash_update(void ** hash, const void * item)
{
  bool found;
  void * stored_item = _ahash_find_or_update(hash, item, &found);
  if (found)
  {
    AHash * ahash = _ahash_header(*hash);
    memcpy(stored_item, item, ahash->item_size);
  }
  return stored_item;
}

static inline bool _ahash_delete(void * hash, const void * key)
{
  AHash * ahash = _ahash_header(hash);
  ArenaHashInt capacity = ahash->capacity;
  uint32_t item_size = ahash->item_size;
  ArenaHashInt * table = ahash->table;
  ArenaHashInt slot = _ahash_find_slot(hash, key);
  if (table[slot] == 0) { return 0; }
  table[slot] = 1;  // tombstone
  ahash->tombstone_count++;
  ArenaHashInt index = table[capacity * 2 + slot];
  ArenaHashInt final_index = ahash->length - 1;
  if (index < final_index)
  {
    // Move the final item to take the place of the deleted item.
    void * item = (uint8_t *)hash + index * item_size;
    void * final_item = (uint8_t *)hash + final_index * item_size;
    memcpy(item, final_item, item_size);
    ArenaHashInt slot2 = _ahash_find_slot(hash, item);
    assert(table[slot2] && table[capacity * 2 + slot2] == final_index);
    table[capacity * 2 + slot2] = index;
  }
  ahash->length--;
  return 1;
}

#define ahash_create(arena, capacity, type, T) ((T *)_ahash_create((arena), (capacity), (type), sizeof(((T*)0)->key), sizeof(T), alignof(T)))
#define ahash_length _ahash_length
#define ahash_capacity _ahash_capacity

#ifdef __cplusplus
template <typename T> static inline T * ahash_copy(const T * hash, size_t capacity)
{
  return (T *)_ahash_copy((const void *)hash, capacity);
}

template<typename T> static inline void ahash_resize_capacity(T * & hash, size_t capacity)
{
  _ahash_resize_capacity((void **)&hash, capacity);
}

template<typename T> static inline void ahash_ensure_space(T * & hash, size_t count)
{
  _ahash_ensure_space((void **)&hash, count);
}

template<typename T> static inline T * ahash_find(const T * hash,
  decltype(((T*)0)->key) key)
{
  return (T *)_ahash_find((const void *)hash, &key);
}

template<typename T> static inline T * ahash_find_p(const T * hash,
  const decltype(((T*)0)->key) * key)
{
  return (T *)_ahash_find((const void *)hash, key);
}

template<typename T> static inline T * ahash_find_or_update(T * & hash,
  const T * item, bool * found)
{
  return (T *)_ahash_find_or_update((void **)&hash, item, found);
}

template<typename T> static inline T * ahash_find_or_update(T * & hash,
  T item, bool * found)
{
  return (T *)_ahash_find_or_update((void **)&hash, &item, found);
}

template<typename T> static inline T * ahash_update(T * & hash, const T * item)
{
  return (T *)_ahash_update((void **)&hash, item);
}

template<typename T> static inline T * ahash_update(T * & hash, T item)
{
  return (T *)_ahash_update((void **)&hash, &item);
}

template<typename T> static inline bool ahash_delete(T * hash,
  decltype(((T*)0)->key) key)
{
  return _ahash_delete(hash, &key);
}

template<typename T> static inline bool ahash_delete_p(T * hash,
  const decltype(((T*)0)->key) * key)
{
  return _ahash_delete(hash, key);
}

#else
#define ahash_copy(hash, cap) ((typeof_unqual(*hash)*)_ahash_copy((hash), (cap)))
#define ahash_resize_capacity(hash, c) (_ahash_resize_capacity(_ARENA_PP(&(hash)), (c)))
#define ahash_ensure_space(hash, c) (_ahash_ensure_space(_ARENA_PP(&(hash)), (c)))
#define ahash_set_length(hash, l) (_ahash_set_length(_ARENA_PP(&(hash)), (l)))
#define ahash_find(hash, k) ((typeof(hash))_ahash_find((hash), _ARENA_T_VAL((k), typeof_unqual((hash)->key))))
#define ahash_find_p(hash, k) ((typeof(hash))_ahash_find((hash), _ARENA_T_PTR((k), typeof_unqual((hash)->key))))
#define ahash_find_or_update(hash, item, f) ((typeof(hash))_ahash_find_or_update(_ARENA_PP(&(hash)), _ARENA_T_PTR_OR_VAL((item), typeof(*hash)), (f)))
#define ahash_update(hash, item) ((typeof(hash))_ahash_update(_ARENA_PP(&(hash)), _ARENA_T_PTR_OR_VAL((item), typeof(*hash))))
#define ahash_delete(hash, k) (_ahash_delete((hash), _ARENA_T_VAL((k), typeof_unqual((hash)->key))))
#define ahash_delete_p(hash, k) (_ahash_delete_p((hash), _ARENA_T_PTR((k), typeof_unqual((hash)->key))))
#endif
