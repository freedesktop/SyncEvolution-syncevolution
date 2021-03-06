/*
 * Copyright (C) 2008 Novell, Inc.
 * Copyright (C) 2009 Patrick Ohly <patrick.ohly@gmx.de>
 *
 * Authors: Patrick Ohly <patrick.ohly@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef E_CAL_CHECK_TIMEZONES_H
#define E_CAL_CHECK_TIMEZONES_H

#include <syncevo/eds_abi_wrapper.h>
/* #include <libical/ical.h> */
#include <glib.h>

G_BEGIN_DECLS

gboolean e_cal_check_timezones(icalcomponent *comp,
                               GList *comps,
                               icaltimezone *(*tzlookup)(const char *tzid,
                                                         const void *custom,
                                                         GError **error),
                               const void *custom,
                               GError **error);

icaltimezone *e_cal_tzlookup_ecal(const char *tzid,
                                  const void *custom,
                                  GError **error);

icaltimezone *e_cal_tzlookup_icomp(const char *tzid,
                                   const void *custom,
                                   GError **error);

const char *e_cal_match_tzid(const char *tzid);

G_END_DECLS

#endif /* E_CAL_CHECK_TIMEZONES_H */
