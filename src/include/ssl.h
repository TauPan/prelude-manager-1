/*****
*
* Copyright (C) 1998 - 2002 Yoann Vandoorselaere <yoann@mandrakesoft.com>
* All Rights Reserved
*
* This file is part of the Prelude program.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by 
* the Free Software Foundation; either version 2, or (at your option)
* any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; see the file COPYING.  If not, write to
* the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*
*****/

#include "config.h"

#ifdef HAVE_SSL

#include <openssl/ssl.h>
#include <libprelude/ssl-gencrypto.h>

#define SENSORS_CERT CONFIG_DIR"/prelude-sensors.cert"
#define MANAGER_KEY CONFIG_DIR"/prelude-manager.key"


int ssl_auth_client(prelude_io_t *fd);

int ssl_init_server(void);

int ssl_create_certificate(config_t *cfg, int crypt_key);

#endif

