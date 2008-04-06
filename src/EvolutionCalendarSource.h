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

#ifndef INCL_EVOLUTIONCALENDARSOURCE
#define INCL_EVOLUTIONCALENDARSOURCE

#include <config.h>
#include "TrackingSyncSource.h"
#include "EvolutionSmartPtr.h"

#ifdef ENABLE_ECAL

#include <libecal/e-cal.h>

/**
 * Implements access to Evolution calendars, either
 * using the to-do item or events. Change tracking
 * is done by looking at the modification time stamp.
 * Recurring events and their detached recurrences are
 * handled as one item for the main event and one item
 * for each detached recurrence.
 */
class EvolutionCalendarSource : public TrackingSyncSource
{
  public:
    /**
     * @param    type        chooses which kind of calendar data to use:
     *                       E_CAL_SOURCE_TYPE_TODO,
     *                       E_CAL_SOURCE_TYPE_JOURNAL,
     *                       E_CAL_SOURCE_TYPE_EVENT
     */
    EvolutionCalendarSource(ECalSourceType type,
                            const EvolutionSyncSourceParams &params);
    EvolutionCalendarSource(const EvolutionCalendarSource &other);
    virtual ~EvolutionCalendarSource() { close(); }

    //
    // implementation of EvolutionSyncSource
    //
    virtual sources getSyncBackends();
    virtual void open();
    virtual void close(); 
    virtual void exportData(ostream &out);
    virtual string fileSuffix() { return "ics"; }
    virtual const char *getMimeType() const { return "text/calendar"; }
    virtual const char *getMimeVersion() const { return "2.0"; }
    virtual const char *getSupportedTypes() const { return "text/calendar:2.0"; }
   
    virtual SyncItem *createItem(const string &luid);

    //
    // implementation of SyncSource
    //
    virtual ArrayElement *clone() { return new EvolutionCalendarSource(*this); }

  protected:
    //
    // implementation of TrackingSyncSource callbacks
    //
    virtual void listAllItems(RevisionMap_t &revisions);
    virtual string insertItem(string &luid, const SyncItem &item, bool &merged);
    virtual void setItemStatusThrow(const char *key, int status);
    virtual void deleteItem(const string &luid);
    virtual void flush();
    virtual void logItem(const string &luid, const string &info, bool debug = false);
    virtual void logItem(const SyncItem &item, const string &info, bool debug = false);

  protected:
    /** valid after open(): the calendar that this source references */
    eptr<ECal, GObject> m_calendar;

    ECalSourceType m_type;         /**< use events or todos? */
    string m_typeName;             /**< "calendar", "task list", "memo list" */
    ECal *(*m_newSystem)(void);    /**< e_cal_new_system_calendar, etc. */
    

    /**
     * An item is identified in the calendar by
     * its UID (unique ID) and RID (recurrence ID).
     * The RID may be empty.
     *
     * This is turned into a SyncML LUID by
     * concatenating them: <uid>-rid<rid>.
     */
    class ItemID {
    public:
    ItemID(const string &uid, const string &rid) :
        m_uid(uid),
            m_rid(rid)
            {}
    ItemID(const char *uid, const char *rid):
        m_uid(uid ? uid : ""),
            m_rid(rid ? rid : "")
                {}

        const string m_uid, m_rid;

        string getLUID() const;
        static string getLUID(const string &uid, const string &rid);
        static ItemID parseLUID(const string &luid);
    };

    /**
     * retrieve the item with the given id - may throw exception
     *
     * caller has to free result
     */
    icalcomponent *retrieveItem(const ItemID &id);

    /** retrieve the item with the given luid as VCALENDAR string - may throw exception */
    string retrieveItemAsString(const ItemID &id);


    /** returns the type which the ical library uses for our components */
    icalcomponent_kind getCompType() {
        return m_type == E_CAL_SOURCE_TYPE_EVENT ? ICAL_VEVENT_COMPONENT :
            m_type == E_CAL_SOURCE_TYPE_JOURNAL ? ICAL_VJOURNAL_COMPONENT :
            ICAL_VTODO_COMPONENT;
    }

    /** ECalAuthFunc which calls the authenticate() methods */
    static char *eCalAuthFunc(ECal *ecal,
                              const char *prompt,
                              const char *key,
                              gpointer user_data) {
        return ((EvolutionCalendarSource *)user_data)->authenticate(prompt, key);
    }

    /** actual implementation of ECalAuthFunc */
    char *authenticate(const char *prompt,
                       const char *key);

    /**
     * Returns the LUID of a calendar item.
     */
    string getLUID(ECalComponent *ecomp);

    /**
     * Extract item ID from calendar item.  An icalcomponent must
     * refer to the VEVENT/VTODO/VJOURNAL component.
     */
    ItemID getItemID(ECalComponent *ecomp);
    ItemID getItemID(icalcomponent *icomp);

    /**
     * Extract modification string from calendar item.
     */
    string getItemModTime(ECalComponent *ecomp);

    /**
     * Extract modification string of an item stored in
     * the calendar.
     */
    string getItemModTime(const ItemID &id);

    /**
     * Convert to string in canonical representation.
     */
    string icalTime2Str(const struct icaltimetype &tt);
};

#else

typedef int ECalSourceType;

#endif // ENABLE_ECAL

#endif // INCL_EVOLUTIONSYNCSOURCE
