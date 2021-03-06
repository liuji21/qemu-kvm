/*
 * Public TPM functions
 *
 * Copyright (C) 2011-2013 IBM Corporation
 *
 * Authors:
 *  Stefan Berger    <stefanb@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QEMU_TPM_H
#define QEMU_TPM_H

#include "qemu/option.h"

typedef struct TPMState TPMState;
typedef struct TPMSizedBuffer TPMSizedBuffer;
typedef void (TPMRecvDataCB)(TPMState *, uint8_t locty);

int tpm_config_parse(QemuOptsList *opts_list, const char *optarg);
int tpm_init(void);
void tpm_cleanup(void);

#endif /* QEMU_TPM_H */
