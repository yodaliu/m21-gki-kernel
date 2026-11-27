/* SPDX-License-Identifier: GPL-2.0 */
/* entry points shared by the three parts of fiio_underlay_modules */
#ifndef _FIIO_UNDERLAY_H
#define _FIIO_UNDERLAY_H

int fiio_attr_init(void);
void fiio_attr_exit(void);
int fiio_load_init(void);
void fiio_load_exit(void);

#endif
