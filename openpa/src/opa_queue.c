/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*  
 *  (C) 2008 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 */

/* FIXME remove MPI error code references in a meaningful way */

#include "opa_queue.h"

char *OPA_Shm_asymm_base_addr = (char *)(OPA_SHM_ASYMM_NULL_VAL);

int OPA_Shm_asymm_init(char *base)
{
    int mpi_errno = 0/*MPI_SUCCESS*/;

    if (OPA_Shm_asymm_base_addr != (char *)OPA_SHM_ASYMM_NULL_VAL) {
        /* the _base_addr has already been initialized */
        mpi_errno = 16/*MPI_ERR_INTERN*/;
        goto fn_exit;
    }

    OPA_Shm_asymm_base_addr = base;

fn_exit:
    return mpi_errno;
}

void OPA_Queue_init(OPA_Queue_info_t *qhead)
{
    OPA_SHM_SET_REL_NULL((qhead)->head);
    OPA_SHM_SET_REL_NULL((qhead)->tail);
    OPA_SHM_SET_REL_NULL((qhead)->shadow_head);
}

void OPA_Queue_header_init(OPA_Queue_element_hdr_t *hdr)
{
    OPA_SHM_SET_REL_NULL(hdr->next);
}

