/* SPDX-License-Identifier: GPL-2.0-or-later */
/*****************************************************************************
  Copyright (c) 2026 DTI Technologies s.r.o.

  Authors: Tomas Kyzlink <tkyzlink@dtitech.cz>

******************************************************************************/

#ifndef EXEC_CMD_H
#define EXEC_CMD_H

#include "io_buffer.h"

struct exec_sec
{
    uid_t uid;
    gid_t gid;
};

int exec_cmd(struct iobuf *in, struct iobuf *out, struct iobuf *err, int *exitcode, struct exec_sec *sec, const char *cmd, const char **params);

#endif /* EXEC_CMD */
