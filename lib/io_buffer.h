/* SPDX-License-Identifier: GPL-2.0-or-later */
/*****************************************************************************
  Copyright (c) 2025 DTI Technologies s.r.o.

  Authors: Tomas Kyzlink <tkyzlink@dtitech.cz>

******************************************************************************/

#ifndef IO_BUFFER_H
#define IO_BUFFER_H

#include <stdio.h>

struct iobuf
{
    size_t size;       /* size of data in buffer */
    size_t pos;        /* position for next read */
    size_t cap;        /* total allocated buffer size */
    char *data;
};

void iobuf_init(struct iobuf *buffer);
void iobuf_cleanup(struct iobuf *buffer);

int iobuf_resize(struct iobuf *buffer, size_t new_size);

int iobuf_read_from_fd(struct iobuf *buffer, int filedes);
int iobuf_write_to_fd(int filedes, struct iobuf *buffer); /* from current pos */

int iobuf_readcleanline(struct iobuf *buffer, char ** begin); /* reads line from file, return value length+1 */

#endif /* IO_BUFFER */
