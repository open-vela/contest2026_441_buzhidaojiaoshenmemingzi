/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
void *bk7258_psram_malloc(size_t size);
void bk7258_psram_free(void *pointer);
size_t bk7258_psram_free_size(void);
size_t bk7258_psram_total_size(void);
