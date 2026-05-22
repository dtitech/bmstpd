/* SPDX-License-Identifier: GPL-2.0-or-later */
/*****************************************************************************
  Copyright (c) 2025 DTI Technologies s.r.o.

  Authors: Tomas Kyzlink <tkyzlink@dtitech.cz>

******************************************************************************/

#ifndef MSTP_CONF_H
#define MSTP_CONF_H

#include "mstp.h"

bool mstpd_conf_exist_br(const char *br_name);

bool mstpd_conf_load_br(bridge_t *br);
bool mstpd_conf_load_prt(port_t *prt);

#endif /* MSTPD_CONF_H */
