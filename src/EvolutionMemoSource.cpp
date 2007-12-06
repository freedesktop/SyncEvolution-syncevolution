/*
 * Copyright (C) 2005-2006 Patrick Ohly
 * Copyright (C) 2007 Funambol
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <memory>
using namespace std;

#include "config.h"

#ifdef ENABLE_ECAL

#include "EvolutionMemoSource.h"
#include "EvolutionSmartPtr.h"

#include <common/base/Log.h>

SyncItem *EvolutionMemoSource::createItem( const string &uid, SyncState state )
{
    logItem( uid, "extracting from EV" );
    GError *gerror = NULL;
    icalcomponent *compptr;
  
    if (!e_cal_get_object(m_calendar,
                          uid.c_str(),
                          NULL,
                          &compptr,
                          &gerror)) {
        throwError(string("retrieving item: ") + uid, gerror);
    }
    if (!compptr) {
        throwError(string("retrieving item: ") + uid);
    }
        
    eptr<icalcomponent> comp(compptr);
    auto_ptr<SyncItem> item(new SyncItem(uid.c_str()));

    item->setData("", 0);
    icalcomponent *cal = icalcomponent_get_first_component(comp, ICAL_VCALENDAR_COMPONENT);
    if (!cal) {
        cal = comp;
    }
    icalcomponent *journal = icalcomponent_get_first_component(cal, ICAL_VJOURNAL_COMPONENT);
    if (!journal) {
        journal = comp;
    }
    icalproperty *desc = icalcomponent_get_first_property(journal, ICAL_DESCRIPTION_PROPERTY);
    if (desc) {
        const char *text = icalproperty_get_description(desc);
        if (text) {
            item->setData(text, strlen(text));
        }
    }
    item->setDataType("text/plain");
    item->setModificationTime(0);
    item->setState(state);

    return item.release();
}

int EvolutionMemoSource::insertItem(SyncItem& item, bool update)
{
    const char *type = item.getDataType();

    // fall back to inserting iCalendar 2.0 if
    // real SyncML server has sent vCalendar 1.0 or iCalendar 2.0
    // or the test system inserts such an item
    if (!type[0] ||
        !strcasecmp(type, "raw") ||
        !strcasecmp(type, "text/x-vcalendar") ||
        !strcasecmp(type, "text/calendar")) {
        return EvolutionCalendarSource::insertItem(item, update);
    }
    
    bool fallback = false;
    eptr<char> text;
    text.set((char *)malloc(item.getDataSize() + 1), "copy of item");
    memcpy(text, item.getData(), item.getDataSize());
    text[item.getDataSize()] = 0;

    const char *eol = strchr(text, '\n');
    string summary;
    if (eol) {
        summary.insert(0, (char *)text, eol - (char *)text);
    } else {
        summary = (char *)text;
    }

    eptr<icalcomponent> subcomp(icalcomponent_vanew(
                                    ICAL_VJOURNAL_COMPONENT,
                                    icalproperty_new_summary(summary.c_str()),
                                    icalproperty_new_description(text),
                                    0));

    if( !subcomp ) {
        throwError( string( "creating vjournal " ) + summary,
                    NULL );
    }

    GError *gerror = NULL;
    char *uid = NULL;
    int status = STC_OK;

    if (!update) {
        if(!e_cal_create_object(m_calendar, subcomp, &uid, &gerror)) {
            if (gerror->domain == E_CALENDAR_ERROR &&
                gerror->code == E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS) {
                // Deal with error due to adding already existing item.
                // Should never happen for plain text journal entries because
                // they have no embedded ID, but who knows...
                logItem(item, "exists already, updating instead");
                fallback = true;
                g_clear_error(&gerror);
            } else {
                throwError( "storing new memo item", gerror );
            }
        } else {
            if (uid) {
                item.setKey(uid);
            }
        }
    }

    if (update || fallback) {
        // ensure that the component has the right UID
        if (update && item.getKey() && item.getKey()[0]) {
            icalcomponent_set_uid(subcomp, item.getKey());
        }
        
        if (!e_cal_modify_object(m_calendar, subcomp, CALOBJ_MOD_ALL, &gerror)) {
            throwError(string("updating memo item ") + item.getKey(), gerror);
        }
        string uid = getCompUID(subcomp);
        if (uid.size()) {
            item.setKey(uid.c_str());
        }
        if (fallback) {
            status = STC_CONFLICT_RESOLVED_WITH_MERGE;
        }
    }

    return status;
}

#endif /* ENABLE_ECAL */
