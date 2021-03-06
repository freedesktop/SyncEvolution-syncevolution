/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "icalstrdup.h"
#include <syncevo/eds_abi_wrapper.h>

#if !defined(LIBICAL_MEMFIXES)

#if defined(HAVE_CONFIG_H)
# include <config.h>
#endif

#if defined(_GNU_SOURCE) && defined(HAVE_DLFCN_H)
# include <dlfcn.h>
# define LIBICAL_RUNTIME_CHECK
#endif

#include <string.h>

char *ical_strdup(const char *x)
{
#ifdef LIBICAL_RUNTIME_CHECK
    // One situation when we must not dup strings is when
    // running with a libecal with the modified string
    // handling semantic. Check that here.
    static enum {
        PATCH_UNCHECKED,
        PATCH_FOUND,
        PATCH_NOT_FOUND
    } patch_status;

    if (patch_status == PATCH_UNCHECKED) {
        patch_status = dlsym(RTLD_NEXT, "ical_memfixes") != NULL ?
            PATCH_FOUND : PATCH_NOT_FOUND;
    }

    if (patch_status == PATCH_FOUND) {
        /* patch applied, no need to copy */
        return (char *)x;
    }
#endif

    return x ? strdup(x) : NULL;
}

#endif /* !LIBICAL_MEMFIXES */
