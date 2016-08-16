#ifndef _BINSEARCH_H
#define _BINSEARCH_H
/**
 * binsearch.h
 *
 * Binary search implementation. This expects a static set of items to search
 * through, with a continuous amount of memory allocated first, then filling
 * it, then searching it. So no online inserts and deletes. But fast lookups,
 * also for queries such as "give me the largest element smaller than x".
 */

#include <stdlib.h>
#include <errno.h>

#undef DBG
#ifdef BINSEARCH_DEBUG
#include <stdio.h>
#define DBG(string, args...) printf("[DBG %s:%s] "string, __FILE__, __func__, ##args)
#else
#define DBG(args...)
#endif /* BINSEARCH_DEBUG */

/* control information at the head of the binary search array */
typedef struct {
	unsigned int num;
} control_block_t;
/* the elements inside the array. One int as "key", one union of an int,
 * character pointer, and void pointer as "value" that can either carry
 * the value directly, or point to somewhere outside the search array for
 * more complicated values.
 */
typedef struct {
	unsigned int key;
	union {
		int value;
		char *c;
		void *p;
	} val;
} element_t;

/** allocate a continuous memory block, starting at head, of size
 *  sizeof(control_block_t) + sizeof(element_t) * num bytes, to
 *  contain num elements, plus a control block at the beginning;
 *  Returns pointer to beginning of array on success, 0 otherwise.
 */
void *binsearch_alloc(unsigned int num)
{
	void *head;
	control_block_t *cb;
	head = malloc(sizeof(control_block_t) + num * sizeof(element_t));
	if (!head)
		return NULL;
	cb = head;
	cb->num = num;
	return head;
}

/**
 * push elements into array allocated with binsearch_alloc.
 * Returns 0 on success, otherwise an error code.
 */
int binsearch_fill(void *head, element_t *ele)
{
	static unsigned int filled = 0;
	control_block_t *cb = head;
	element_t *put = head + sizeof(control_block_t) + filled * sizeof(element_t);
	DBG("filling array position %u\n", filled);

	if (++filled > cb->num)
		return -ENOMEM;

	*put = *ele;
	return 0;
}

element_t *__binsearch_find_exact(void *head, unsigned int key, unsigned int first, unsigned int last)
{
	unsigned int median = first + (last-first)/2;
	element_t *fele = (element_t *)(head + sizeof(control_block_t) + first * sizeof(element_t));
	element_t *lele = (element_t *)(head + sizeof(control_block_t) + last * sizeof(element_t));
	element_t *mele = (element_t *)(head + sizeof(control_block_t) + median * sizeof(element_t));

	DBG("search array %p from element %u (key %u) to %u (key %u)\n", head, first, fele->key, last, lele->key);
	if (key == fele->key)
		return fele;
	else if (key < fele->key)
		return NULL;
	else if (key == lele->key)
		return lele;
	else if (key > lele->key)
		return NULL;
	else if (key == mele->key)
		return mele;
	else if (key < mele->key)
		return __binsearch_find_exact(head, key, first, median-1);
	else
		return __binsearch_find_exact(head, key, median+1, last);
}

element_t *__binsearch_find_not_above(void *head, unsigned int key, unsigned int first, unsigned int last)
{
	unsigned int median = first + (last-first)/2;
	element_t *fele = (element_t *)(head + sizeof(control_block_t) + first * sizeof(element_t));
	element_t *lele = (element_t *)(head + sizeof(control_block_t) + last * sizeof(element_t));
	element_t *mele = (element_t *)(head + sizeof(control_block_t) + median * sizeof(element_t));

	DBG("search array %p from element %u (key %u) to %u (key %u)\n", head, first, fele->key, last, lele->key);
	if (key < fele->key)
		return NULL;
	if ((key >= fele->key) && (key < (fele+1)->key))
		return fele;
	else if (key >= lele->key)
		return lele;
	else if (key < mele->key)
		return __binsearch_find_not_above(head, key, first, median-1);
	else
		return __binsearch_find_not_above(head, key, median, last);
}

element_t *binsearch_find_exact(void *head, unsigned int key)
{
	control_block_t *cb = head;
	return __binsearch_find_exact(head, key, 0, cb->num-1);
}

element_t *binsearch_find_not_above(void *head, unsigned int key)
{
	control_block_t *cb = head;
	return __binsearch_find_not_above(head, key, 0, cb->num-1);
}

#ifdef BINSEARCH_DEBUG
void binsearch_debug_dump_array(void *head)
{
	unsigned int num = ((control_block_t *)head)->num;
	element_t *ele;
	unsigned int i;

	printf("binary search array starting at %p can contain %u elements\n",head, num);
	for (i=0; i < num; i++) {
		ele = (element_t *)(head + sizeof(control_block_t) + (i * sizeof(element_t)));
		printf("Element %u contains key %u->%s\n", i, ele->key, ele->val.c);
	}
}
#endif /* BINSEARCH_DEBUG */


#endif /* _BINSEARCH_H */
