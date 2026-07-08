#ifndef COLLECTIONS_H
#define COLLECTIONS_H

///////////////////////////////////////
// generic container mini-library used internally by minui
// (extracted verbatim from minui.c, no domain knowledge)
///////////////////////////////////////

typedef struct Array {
	int count;
	int capacity;
	void** items;
} Array;

Array* Array_new(void);
void Array_push(Array* self, void* item);
void Array_unshift(Array* self, void* item);
void* Array_pop(Array* self);
void Array_free(Array* self);

void StringArray_free(Array* self);

///////////////////////////////////////

typedef struct Hash {
	Array* keys;
	Array* values;
} Hash; // not really a hash

Hash* Hash_new(void);
void Hash_free(Hash* self);
void Hash_set(Hash* self, char* key, char* value);
char* Hash_get(Hash* self, char* key);

///////////////////////////////////////

#define INT_ARRAY_MAX 27
typedef struct IntArray {
	int count;
	int items[INT_ARRAY_MAX];
} IntArray;

IntArray* IntArray_new(void);
void IntArray_push(IntArray* self, int i);
void IntArray_free(IntArray* self);

#endif
