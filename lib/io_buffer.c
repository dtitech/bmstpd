/* SPDX-License-Identifier: GPL-2.0-or-later */
/*****************************************************************************
  Copyright (c) 2025 DTI Technologies s.r.o.

  Authors: Tomas Kyzlink <tkyzlink@dtitech.cz>

******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "io_buffer.h"

#define IO_PAGE_SIZE         4096

static const size_t iobuf_round_to_page_size(size_t size)
{
    return ((size + (IO_PAGE_SIZE - 1)) / IO_PAGE_SIZE) * IO_PAGE_SIZE;
}

void iobuf_init(struct iobuf *buffer)
{
    buffer->size = 0;
    buffer->pos = 0;
    buffer->cap = 0;
    buffer->data = NULL;
}

void iobuf_cleanup(struct iobuf *buffer)
{
    buffer->size = 0;
    buffer->pos = 0;
    buffer->cap = 0;
    if (buffer->data)
    {
        free(buffer->data);
        buffer->data = NULL;
    }
}

int iobuf_resize(struct iobuf *buffer, size_t new_size)
{
    if (buffer->size >= new_size)
        return 0;

    const size_t new_cap = iobuf_round_to_page_size(new_size);

    void *new_data = realloc(buffer->data, new_cap);
    if (!new_data)
    {
        errno = ENOMEM; // to be sure
        return -1;
    }

    buffer->data = (char *)new_data;
    buffer->cap = new_cap;
    return 0;
}

int iobuf_write(struct iobuf *buffer, const void *data, int size)
{
    if (!size)
        return 0;

    size_t bfree = buffer->cap - buffer->size;
    if (bfree < size)
    {
        const size_t esize = iobuf_round_to_page_size(buffer->cap + size);
        if (iobuf_resize(buffer, esize))
            return -1;
    }

    memcpy(&(buffer->data[buffer->size]), data, size);
    buffer->size += size;

    return size;
}

int iobuf_read_from_fd(struct iobuf *buffer, int filedes)
{
    size_t bfree = buffer->cap - buffer->size;
    if (bfree < 1024)
    {
        const size_t esize = iobuf_round_to_page_size(buffer->cap + 1024);
        if (iobuf_resize(buffer, esize))
            return -1;

        bfree = buffer->cap - buffer->size;
    }
    ssize_t rd = read(filedes, &(buffer->data[buffer->size]), bfree);
    if (rd > 0)
    {
        buffer->size += rd;
        return rd;
    }
    else if (rd == 0)
        return 0;

    return -1;
}

int iobuf_read_from_fd_sz(struct iobuf *buffer, int filedes, int size)
{
    if (!size)
        return -1;

    if ((buffer->cap - buffer->size) < size)
    {
        const size_t esize = iobuf_round_to_page_size(buffer->cap + size);
        if (iobuf_resize(buffer, esize))
            return -1;
    }
    ssize_t rd = read(filedes, &(buffer->data[buffer->size]), size);
    if (rd > 0)
    {
        buffer->size += rd;
        return rd;
    }
    else if (rd == 0)
        return 0;

    return -1;
}

int iobuf_read_from_fd_all(struct iobuf *buffer, int filedes)
{
    for (;;)
    {
        size_t bfree = buffer->cap - buffer->size;
        if (bfree < 1024)
        {
            const size_t esize = iobuf_round_to_page_size(buffer->cap + 1024);
            if (iobuf_resize(buffer, esize))
                return -1;

            bfree = buffer->cap - buffer->size;
        }
        ssize_t rd = read(filedes, &(buffer->data[buffer->size]), bfree);
        if (rd > 0)
            buffer->size += rd;
        else if (rd == 0)
            break;
        else
            return -1;
    }

    return 0;
}

int iobuf_write_to_fd(int filedes, struct iobuf *buffer)
{
    size_t wsize = buffer->size - buffer->pos;
    if (!wsize)
        return 0;

    ssize_t wr = write(filedes, &buffer->data[buffer->pos], wsize);
    if (wr < 0)
        return -1;

    buffer->pos += wr;
    return wr;
}

int iobuf_write_to_fd_sz(int filedes, struct iobuf *buffer, int size)
{
    size_t wsize = buffer->size - buffer->pos;
    if (!wsize)
        return 0;

    if (wsize > size)
        wsize = size;

    ssize_t wr = write(filedes, &buffer->data[buffer->pos], wsize);
    if (wr < 0)
        return -1;

    buffer->pos += wr;
    return wr;
}

int iobuf_write_to_fd_all(int filedes, struct iobuf *buffer)
{
    for (;;)
    {
        size_t wsize = buffer->size - buffer->pos;
        if (!wsize)
            return 0;

        if (wsize > 65536)
            wsize = 65536;

        ssize_t wr = write(filedes, &buffer->data[buffer->pos], wsize);
        if (wr < 0)
            return -1;

        buffer->pos += wr;
    }

    return 0;
}

int iobuf_readcleanline(struct iobuf *buffer, char **begin)
{
    size_t bpos = buffer->pos;
    size_t bsize = buffer->size;

    if (bpos >= bsize)
        return -1;

    *begin = &buffer->data[bpos];
    size_t bbegin = bpos;

    for (; bpos < bsize; bpos++)
    {
        const char bchar = buffer->data[bpos];
        if (bchar == 0x0A)
            break;
    }

    buffer->pos = (bpos < bsize) ? bpos + 1 : bpos;

    for (; bpos > bbegin; bpos--)
    {
        const char bchar = buffer->data[bpos - 1];
        if ((bchar != 0x0D) && (bchar != 0x09) && (bchar != 0x20))
            break;
    }

    size_t blen = bpos - bbegin;
    return blen + 1;
}
