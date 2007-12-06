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
#include "EvolutionSyncSource.h"
#include "EvolutionSmartPtr.h"

#ifdef ENABLE_ECAL

#include <libecal/e-cal.h>

/**
 * Implements access to Evolution calendars, either
 * using the to-do item or events.
 */
class EvolutionCalendarSource : public EvolutionSyncSource
{
  public:
    /**
     * Creates a new Evolution calendar source.
     *
     * @param    type        chooses which parts of the calendar to use
     * @param    changeId    is used to track changes in the Evolution backend;
     *                       not specifying it implies that always all items are returned
     * @param    id          identifies the backend; not specifying it makes this instance
     *                       unusable for anything but listing backend databases
     */
    EvolutionCalendarSource( ECalSourceType type,
                             const string &name,
                             SyncSourceConfig *sc,
                             const string &changeId = string(""),
                             const string &id = string("") );
    EvolutionCalendarSource( const EvolutionCalendarSource &other );
    virtual ~EvolutionCalendarSource() { close(); }

    //
    // implementation of EvolutionSyncSource
    //
    virtual sources getSyncBackends();
    virtual void open();
    virtual void close(); 
    virtual void exportData(ostream &out);
    virtual string fileSuffix() { return "ics"; }
    virtual const char *getMimeType() { return "text/calendar"; }
    virtual const char *getMimeVersion() { return "2.0"; }
    virtual const char *getSupportedTypes() { return "text/calendar:2.0"; }
   
    virtual SyncItem *createItem( const string &uid, SyncState state );

    //
    // implementation of SyncSource
    //
    virtual ArrayElement *clone() { return new EvolutionCalendarSource(*this); }

  protected:
    //
    // implementation of EvolutionSyncSource callbacks
    //
    virtual void beginSyncThrow(bool needAll,
                                bool needPartial,
                                bool deleteLocal);
    virtual void endSyncThrow();
    virtual void setItemStatusThrow(const char *key, int status);
    virtual int addItemThrow(SyncItem& item);
    virtual int updateItemThrow(SyncItem& item);
    virtual int deleteItemThrow(SyncItem& item);
    virtual void logItem(const string &uid, const string &info, bool debug = false);
    virtual void logItem(SyncItem &item, const string &info, bool debug = false);

  protected:
    /** valid after open(): the calendar that this source references */
    eptr<ECal, GObject> m_calendar;

    ECalSourceType m_type;         /**< use events or todos? */
    string m_typeName;             /**< "calendar", "task list", "memo list" */
    ECal *(*m_newSystem)(void);    /**< e_cal_new_system_calendar, etc. */
    
    /** retrieve the item with the given uid as VCALENDAR string - may throw exception */
    string retrieveItemAsString(const string &uid);

    /**
     * - parse the data stored in the given SyncItem, throw error if cannot be parsed
     * - then either insert or update it, trying update if insert fails because it exists already
     * - also import timezones
     */
    virtual int insertItem(SyncItem &item, bool update);

    /** returns the type which the ical library uses for our components */
    icalcomponent_kind getCompType() {
        return m_type == E_CAL_SOURCE_TYPE_EVENT ? ICAL_VEVENT_COMPONENT :
            m_type == E_CAL_SOURCE_TYPE_JOURNAL ? ICAL_VJOURNAL_COMPONENT :
            ICAL_VTODO_COMPONENT;
    }

    /** returns the uid of the given component */
    string getCompUID(icalcomponent *icomp);

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

    /** help function which resets the lists of added/updated/deleted items and fills them again */
    void getChanges();
};

#else

typedef int ECalSourceType;

#endif // ENABLE_ECAL

#endif // INCL_EVOLUTIONSYNCSOURCE
