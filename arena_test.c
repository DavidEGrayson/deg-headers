// Test suite for arena.h, compilable in both C and C++

#define ARENA_FIRST_BLOCK_SIZE 32
#define ARENA_SMALL_STRING_SIZE 1

#include "arena.h"
#include <time.h>
#include <ctype.h>

typedef struct AllocRequest {
  size_t size;
  size_t alignment;
} AllocRequest;

typedef struct Foo {
  int a, b;
} Foo;

Arena arena;

#define ALLOC_REQUEST_COUNT 32
AllocRequest requests[ALLOC_REQUEST_COUNT];

void hexdump_arena_block(ArenaBlockHeader * block)
{
  printf("Block at %p, size %zu:\n", block, block->size);
  const size_t chunk_size = 16;
  assert((block->size % chunk_size) == 0);
  for (uintptr_t p = (uintptr_t)block; p < (uintptr_t)block + block->size; p += chunk_size)
  {
    printf("%p:", (void *)p);
    uint8_t * chunk = (uint8_t *)p;
    for (size_t i = 0; i < chunk_size; i++)
    {
      if (i == 8) { putchar(' '); }
      printf(" %02x", chunk[i]);
    }
    printf("  |");
    for (size_t i = 0; i < chunk_size; i++)
    {
      if (isprint(chunk[i]))
      {
        putchar(chunk[i]);
      }
      else
      {
        putchar('.');
      }
    }
    putchar('|');
    putchar('\n');
  }
}

void hexdump_arena(Arena * arena)
{
  printf("Dump of Arena at %p\n", arena);
  ArenaBlockHeader * block = arena->block;
  while (block)
  {
    hexdump_arena_block(block);
    block = block->prev;
  }
}

void hexdump_ahash(void * hash)
{
  printf("Dump of AHash at %p:\n", hash);
  AHash * ahash = _ahash_header(hash);
  printf("  item_size = %u\n", ahash->item_size);
  printf("  key_size = %u\n", ahash->key_size);

  for (ArenaHashInt slot = 0; slot < ahash->capacity * 2; slot++)
  {
    if (ahash->table[slot])
    {
      printf("  slot %u: hash %u -> index %u\n", slot,
        ahash->table[slot], ahash->table[slot + ahash->capacity * 2]);
    }
    else
    {
      printf("  slot %u: empty\n", slot);
    }
  }

  for (ArenaHashInt index = 0; index <= ahash->length; index++)
  {
    printf("  index %u:", index);
    for (uint32_t i = 0; i < ahash->key_size; i++)
    {
      printf(" %02x", *((uint8_t *)hash + index * ahash->item_size + i));
    }
    printf("\n");
  }
}

// The string functions require vsnprintf to have the proper return value.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
void test_vsnprintf()
{
  va_list ap = {0};
  char s[4] = {};
  int r = vsnprintf(s, 2, "abcd", ap);
  if (r != 4 || strcmp(s, "a"))
  {
    fprintf(stderr, "vsnprintf test failed (%d,%s)\n", r, s);
    exit(1);
  }
}
#pragma GCC diagnostic pop

void test_c_typechecking()
{
#ifndef __cplusplus
  (void)_ARENA_PP((int **)NULL);
  //(void)_ARENA_PP((const int **)NULL);   // error in C
  //(void)_ARENA_PP((int * const *)NULL);  // error in C
  (void)_ARENA_PP((int ** const)NULL);  // error in C?
#endif
}

void test_arena_randomly()
{
  for (size_t i = 0; i < ALLOC_REQUEST_COUNT; i++)
  {
    unsigned int r = rand();
    uint8_t ae = rand() >> 8 & 7;
    if (ae >= 4) { ae = 3; }
    requests[i].alignment = 1 << ae;
    requests[i].size = ((r & 31) + 1) * requests[i].alignment;
  }

  void * last_alloc = NULL;
  size_t last_alloc_size = 0;
  arena_clear(&arena);
  for (size_t i = 0; i < ALLOC_REQUEST_COUNT; i++)
  {
    void * alloc = arena_alloc(&arena, requests[i].size, requests[i].alignment);
    if (alloc == last_alloc)
    {
      fprintf(stderr, "Error: Same address returned twice.");
      exit(1);
    }
    if ((uintptr_t)alloc - (uintptr_t)last_alloc < last_alloc_size)
    {
      fprintf(stderr, "Error: Allocation overlaps previous allocation.");
      exit(1);
    }

    last_alloc = alloc;
    last_alloc_size = requests[i].size;
  }

  arena_free(&arena);
}

void test_arena_printf()
{
  char * hi = arena_printf(&arena, "hi!!!!!!!!");
  char * david = arena_printf(&arena, "David");
  assert(strcmp(hi, "hi!!!!!!!!") == 0);
  assert(strcmp(david, "David") == 0);
}

void test_astring()
{
  char * str1 = astr_create_f(&arena, "hi! %d", 1);
  assert(0 == strcmp("hi! 1", str1));

  char * str2 = astr_create_f(&arena, "Yep!");
  char * str3 = astr_copy(str2, 0);
  astr_puts(&str3, " Expand.");
  assert(0 == strcmp("Yep!", str2));
  assert(0 == strcmp("Yep! Expand.", str3));

  //astr_clear(str2);  // error in C/C++

  // astr_set_length tests
  {
    char * str = astr_create(&arena, 8);
    assert(astr_capacity(str) == 8);
    memset(str, 'a', 8);
    astr_set_length(&str, 4);  // grow string from 0 to 4
    assert(str[0] == 0);
    assert(str[1] == 0);
    assert(str[2] == 0);
    assert(str[3] == 0);
    assert(str[4] == 0);
    assert(str[5] == 'a');
    str[0] = 'b';
    str[1] = 'b';
    astr_set_length(&str, 1);  // shrink string from 4 to 1
    assert(str[0] == 'b');
    assert(str[1] == 0);
    str[1] = 'c';
    str[2] = 'c';
    astr_set_length(&str, 10);  // grow string from 1 to 10, increasing capacity
    assert(str[0] == 'b');
    assert(str[1] == 0);
    assert(str[2] == 0);
    assert(str[9] == 0);
    assert(str[10] == 0);
    assert(astr_capacity(str) == 10);
  }

  char * str4 = astr_create_f(&arena, "abcd");
  char * str4_copy;

  // AString const tests
  {
    const char * cstr = str4;
    assert(astr_length(cstr) == 4);
    assert(astr_capacity(cstr) == 4);
    //astr_set_length(&cstr, 0);  // error in C/C++
    str4_copy = astr_copy(cstr, 0);
  }

  {
    const char * const cstr = str4;
    assert(astr_length(cstr) == 4);
    assert(astr_capacity(cstr) == 4);
    //astr_set_length(&cstr, 0);  // error in C/C++
    str4_copy = astr_copy(cstr, 0);
  }

  (void)str4_copy;
}

void test_ali_pointers()
{
  //ali_resize_capacity((Foo *)NULL, 0);  // error in C/C++

  assert(ali_length(NULL) == 0);

  Foo ** foo_list = ali_create(&arena, 1, Foo *);

  // ali_set_length(foo_list, 1);  // missing & but only fails at runtime :(

  Foo * foo0 = arena_alloc1(&arena, Foo);
  ali_push(&foo_list, foo0);

  assert(ali_length(foo_list) == 1);
  assert(ali_capacity(foo_list) == 1);
  assert(foo_list[0] == foo0);
  assert(foo_list[1] == NULL);

  Foo * foo1 = arena_alloc1(&arena, Foo);
  ali_push(&foo_list, foo1);
  assert(ali_length(foo_list) == 2);
  assert(ali_capacity(foo_list) == 4);
  assert(foo_list[0] == foo0);
  assert(foo_list[1] == foo1);
  assert(foo_list[2] == NULL);

  Foo ** foo_list2 = ali_copy(foo_list, 0);

  ali_set_length(&foo_list, 1);
  assert(foo_list[0] == foo0);
  assert(foo_list[1] == NULL);

  assert(foo_list2[1] == foo1);

  ali_resize_capacity(&foo_list, 8);
  assert(ali_capacity(foo_list) == 8);

  // AList const tests: so many cases to test!

  Foo ** copy_list;
  const Foo ** copy_list_const;

  {
    // 000: nothing about the list is const
    //ali_push(&foo_list, (const Foo *)foo0);     // warning in C, error in C++
  }

  {
    // 001: const list
    Foo ** const list = ali_copy(foo_list2, 0);
    assert(ali_length(list) == 2);
    //ali_push(&list, foo0);     // error in C/C++
    copy_list_const = (const Foo **)ali_copy(list, 0);  // safe, but cast required
    copy_list = ali_copy(list, 0);
  }

  {
    // 010: const items
    Foo * const * list = ali_copy(foo_list2, 0);
    assert(ali_length(list) == 2);
    //ali_push(&list, foo1);     // error in C/C++
    copy_list_const = (const Foo **)ali_copy(list, 0);  // safe, but cast required
    copy_list = ali_copy(list, 0);  // warning in C/C++
  }

  {
    // 011: const list and items
    Foo * const * const list = ali_copy(foo_list2, 0);
    assert(ali_length(list) == 2);
    //ali_push(&list, foo1);     // error in C/C++
    copy_list_const = (const Foo **)ali_copy(list, 0);  // safe, but cast required
    copy_list = ali_copy(list, 0);
  }

  {
    // 100: underlying objects const
    const Foo ** list = (const Foo **)ali_copy(foo_list2, 0);
    assert(ali_length(list) == 2);
    ali_set_length(&list, 0);
    ali_push(&list, foo1);
    ali_push(&list, (const Foo *)foo1);
    copy_list_const = ali_copy(list, 0);
    // copy_list = ali_copy(list, 0);   // error in C/C++
  }

  {
    // 111: everything const
    const Foo * const * const list = (const Foo **)foo_list2;
    assert(ali_length(list) == 2);
    //ali_push(&list, foo1);     // error in C/C++
    copy_list_const = ali_copy(list, 0);
    //copy_list = ali_copy(list, 0);    // error in C/C++
  }

  (void)copy_list;
  (void)copy_list_const;
}

void test_ali_ints()
{
  int * int_list = ali_create(&arena, 3, int);

  ali_push(&int_list, 1);
  ali_push(&int_list, (char)2);
  ali_push(&int_list, (int64_t)3);

  assert(ali_length(int_list) == 3);
  assert(ali_capacity(int_list) == 3);
  assert(int_list[0] == 1);
  assert(int_list[1] == 2);
  assert(int_list[2] == 3);
  assert(int_list[3] == 0);

  int * int_list2 = ali_copy(int_list, 0);

  ali_set_length(&int_list, 1);
  assert(int_list[0] == 1);
  assert(int_list[1] == 0);

  assert(int_list2[1] == 2);

  // AList<int> const tests

  int * copy_list;
  const int * copy_list_const;

  {
    // const list
    int * const list = ali_copy(int_list2, 0);
    assert(ali_length(list) == 3);
    //ali_push(&list, foo0);     // error in C/C++
    copy_list_const = ali_copy(list, 0);
    copy_list = ali_copy(list, 0);
  }

  {
    // const items
    const int * list = ali_copy(int_list2, 0);
    assert(ali_length(list) == 3);
    //ali_push(&list, foo1);     // error in C/C++
    copy_list_const = ali_copy(list, 0);
    copy_list = ali_copy(list, 0);
  }

  {
    // const list and items
    const int * const list = ali_copy(int_list2, 0);
    assert(ali_length(list) == 3);
    //ali_push(&list, foo1);     // error in C/C++
    copy_list_const = ali_copy(list, 0);
    copy_list = ali_copy(list, 0);
  }

  (void)copy_list;
  (void)copy_list_const;
}

void test_ali_const_char_p()
{
  const char ** string_list = ali_create(&arena, 3, const char *);
  ali_push(&string_list, "hi");
  const char * str2 = "str2";
  ali_push(&string_list, str2);
}

typedef struct KVPair {
  int key;
  int value;
} KVPair;

void test_ahash_type_default()
{
  KVPair * hash = ahash_create(&arena, 4, AKEY_DEFAULT, KVPair);
  bool found = true;

  {
    // find_or_update with KVPair *, that does an update
    KVPair tmp = { 1, 11 };
    KVPair * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(!found);
    assert(ahash_length(hash) == 1);
    assert(result->key == 1 && result->value == 11);
  }

  {
    // find_or_update with KVPair, that does another update
    KVPair * result = ahash_find_or_update(&hash, ((KVPair){ 2, 22 }), &found);
    assert(!found);
    assert(ahash_length(hash) == 2);
    assert(result->key == 2 && result->value == 22);
  }

  {
    // find_or_update that does not update
    KVPair tmp = { 2, 23 };
    KVPair * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(found);
    assert(ahash_length(hash) == 2);
    assert(ahash_capacity(hash) == 4);
    assert(result->key == 2 && result->value == 22);
  }

  {
    // simple update
    KVPair tmp = { 3, 33 };
    ahash_update(&hash, &tmp);
    assert(ahash_length(hash) == 3);
  }

  {
    // find tests
    assert(ahash_find(hash, -1) == NULL);
    assert(ahash_find(hash, 2)->value == 22);
    int key = 1;
    assert(ahash_find_p(hash, &key)->value == 11);
    int ckey = 2;
    assert(ahash_find_p(hash, &ckey)->value == 22);
  }

  ahash_resize_capacity(&hash, 17);
  assert(ahash_capacity(hash) == 32);

  {
    // Copy tests.
    KVPair * hash2 = ahash_copy(hash, 0);
    assert(ahash_find(hash, 3)->value == 33);
    assert(ahash_find(hash2, 3)->value == 33);
  }

  {
    // Const hashes
    const int cint = 2;
    const KVPair cpair = { 4, 44 };

    // 'const T *' is the typical type of a read-only hash.
    const KVPair * chash = hash;
    ahash_length(chash);
    ahash_capacity(chash);
    assert(ahash_find(chash, 3)->value == 33);
    assert(ahash_find_p(chash, &cint)->value == 22);
    KVPair * copy = ahash_copy(chash, 0);
    assert(ahash_length(copy) == 3);
    //ahash_find_or_update(&chash, chash[0], NULL);  // error in C/C++
    //ahash_update(&chash, &cpair);  // error in C/C++

    // 'T * const' is another type of read-only hash: you would see this
    // if the hash is a member of a struct and you are accessing the struct
    // through a const pointer, for example.
    KVPair * const chash2 = hash;
    ahash_length(chash2);
    ahash_capacity(chash2);
    assert(ahash_find(chash2, 3)->value == 33);
    assert(ahash_find_p(chash2, &cint)->value == 22);
    KVPair * copy2 = ahash_copy(chash2, 0);
    assert(ahash_length(copy2) == 3);
    //ahash_find_or_update(&chash2, chash2[0], NULL);  // error in C/C++
    //ahash_update(&chash2, &cpair);  // error in C/C++

    // If we change the original hash, the read-only hashes see the copy.
    ahash_update(&hash, &cpair);
    assert(ahash_find(chash, 4)->value == 44);
    assert(ahash_find(chash2, 4)->value == 44);
  }
}

typedef struct Intern {
  const char * key;
} Intern;

void test_ahash_type_string()
{
  Intern * hash = ahash_create(&arena, 4, AKEY_STRING, Intern);
  assert(ahash_length(hash) == 0);
  bool found = true;

  char str1[] = "abcd";
  char str2[] = "def";

  {
    Intern tmp = { str1 };
    Intern * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(!found);
    assert(ahash_length(hash) == 1);
    assert(result->key == str1);
  }

  {
    Intern tmp =  { str2 };
    Intern * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(!found);
    assert(ahash_length(hash) == 2);
    assert(result->key == str2);
  }

  {
    Intern tmp = { "abcd" };
    Intern * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(found);
    assert(ahash_length(hash) == 2);
    assert(ahash_capacity(hash) == 4);
    assert(result->key == str1);
  }

  {
    Intern tmp = { "ghi" };
    ahash_find_or_update(&hash, &tmp, &found);
    assert(!found);
    assert(ahash_length(hash) == 3);
  }

  {
    assert(ahash_find(hash, "a") == NULL);
    const char * key = "abcd";
    assert(ahash_find(hash, key)->key == str1);
    assert(ahash_find_p(hash, &key)->key == str1);
  }

  assert(hash[0].key == str1);
  assert(hash[1].key == str2);
  assert(strcmp(hash[2].key, "ghi") == 0);
  assert(hash[3].key == NULL);
}

typedef struct BSVPair
{
  AByteSlice key;
  int value;
} BSVPair;

void test_ahash_type_byte_slice()
{
  BSVPair * hash = ahash_create(&arena, 4, AKEY_BYTE_SLICE, BSVPair);
  assert(ahash_length(hash) == 0);
  bool found = true;

  char str1[] = "abcd";
  char str2[] = "def";

  {
    BSVPair tmp = { { (uint8_t *)str1, strlen(str1) }, 11 };
    BSVPair * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(!found);
    assert(ahash_length(hash) == 1);
    assert(result->key.data == (uint8_t *)str1);
  }

  {
    BSVPair tmp =  { { (uint8_t *)str2, strlen(str2) }, 22 };
    BSVPair * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(!found);
    assert(ahash_length(hash) == 2);
    assert(result->key.data == (uint8_t *)str2);
  }

  {
    BSVPair tmp = { { (uint8_t *)"abcd", 4 }, 12 };
    BSVPair * result = ahash_find_or_update(&hash, &tmp, &found);
    assert(found);
    assert(ahash_length(hash) == 2);
    assert(ahash_capacity(hash) == 4);
    assert(result->key.data == (uint8_t *)str1 && result->value == 11);
  }
}

int main()
{
  srand(time(NULL));

  test_vsnprintf();

  test_c_typechecking();

  test_arena_randomly();

  test_arena_printf();

  test_astring();

  test_ali_pointers();
  test_ali_ints();
  test_ali_const_char_p();

  test_ahash_type_default();
  test_ahash_type_string();
  test_ahash_type_byte_slice();

  printf("Success.\n");

  //hexdump_arena(&arena);
}

// TODO: new brace style to save LOC (opening brace doesn't always get its own line)