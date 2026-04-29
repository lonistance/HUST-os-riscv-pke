#ifndef _PMM_H_
#define _PMM_H_

#include "util/types.h"

// Initialize phisical memeory manager
void pmm_init();
// Allocate a free phisical page
void* alloc_page();
// Free an allocated page
void free_page(void* pa);
// Increase/Decrease/Get page reference count
void incr_page_ref(uint64 pa);
void decr_page_ref(uint64 pa);
int get_page_ref(uint64 pa);
void set_page_ref(uint64 pa, int val);

#endif