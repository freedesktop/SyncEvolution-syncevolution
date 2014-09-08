/*
 * Copyright (C) 2012 Intel Corporation
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

/**
 * The classes in this file implement sorting, searching and
 * configuring of the unified address book based on libfolks.
 *
 * This is pure C++ code. The D-Bus IPC binding for it is
 * implemented separately in pim-manager.h/cpp.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_IVI_FOLKS
#define INCL_SYNCEVO_DBUS_SERVER_IVI_FOLKS

#include <folks/folks.h>

#include "locale-factory.h"
#include "../dbus-callbacks.h"
#include "../timeout.h"

#include <syncevo/GLibSupport.h>
#include <syncevo/GeeSupport.h>
#include <syncevo/GValueSupport.h>

#include <boost/shared_ptr.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/signals2.hpp>

SE_GOBJECT_TYPE(FolksIndividualAggregator)
SE_GOBJECT_TYPE(FolksIndividual)
SE_GOBJECT_TYPE(FolksEmailFieldDetails)
SE_GOBJECT_TYPE(FolksBackendStore)
SE_GOBJECT_TYPE(FolksBackend)
SE_GOBJECT_TYPE(FolksPersonaStore)
SE_GOBJECT_TYPE(FolksAbstractFieldDetails)
SE_GOBJECT_TYPE(FolksRoleFieldDetails)
SE_GOBJECT_TYPE(FolksRole)
SE_GOBJECT_TYPE(FolksPostalAddress)
SE_GOBJECT_TYPE(FolksNoteFieldDetails)
SE_GOBJECT_TYPE(FolksPostalAddressFieldDetails)
SE_GOBJECT_TYPE(FolksPersona)
SE_GOBJECT_TYPE(FolksLocation)
SE_GOBJECT_TYPE(GeeHashSet)

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class PersonaDetails;

/**
 * Abstract interface for comparing two FolksIndividual instances.
 * The properties of a folks individual may change at any time.
 * Therefore the key properties which determine the sort order
 * must be copied from the individual. They will be updated when
 * the individual changes.
 *
 * The other advantage is that complex, derived keys only need
 * to be computed once instead of each time during a comparison.
 * For example, boost::locale::collator::transform() can be used
 * to generate the keys.
 */
class IndividualCompare
{
 public:
    static boost::shared_ptr<IndividualCompare> defaultCompare();

    virtual ~IndividualCompare() {}

    typedef std::vector<std::string> Criteria_t;

    /**
     * Calculate an ordered set of criteria for comparing
     * individuals. The default comparison will start with the initial
     * criteria and move on until a difference is found.
     *
     * This is necessary because a single string of "Doe, John" is
     * treated differently in a collation than the pair of strings
     * "Doe" and "John".
     *
     * @param individual    the individual for which sort keys are to be created
     * @retval criteria     cleared before the call, filled afterwards
     */
    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const = 0;

    /**
     * Partial sort order: true if a smaller than b.
     *
     * Default implementation uses normal std::string::compare().
     */
    virtual bool compare(const Criteria_t &a, const Criteria_t &b) const;
};

/**
 * A FolksIndividual plus its sort criteria and search cache.
 */
struct IndividualData
{
    /**
     * Sets all members to match the given individual, using the
     * compare instance to compute values. Both compare and locale may
     * be NULL.
     *
     * Returns true if the precomputed values changed.
     */
    bool init(const IndividualCompare *compare,
              const LocaleFactory *locale,
              FolksIndividual *individual);

    FolksIndividualCXX m_individual;
    IndividualCompare::Criteria_t m_criteria;
    LocaleFactory::Precomputed m_precomputed;
};

/**
 * wraps an IndividualCompare for std algorithms on IndividualData
 */
class IndividualDataCompare : public std::binary_function<IndividualData, IndividualData, bool>
{
    boost::shared_ptr<IndividualCompare> m_compare;

 public:
    IndividualDataCompare(const boost::shared_ptr<IndividualCompare> &compare) :
       m_compare(compare)
    {}
    IndividualDataCompare(const IndividualDataCompare &other) :
       m_compare(other.m_compare)
    {}

    bool operator () (const IndividualData &a, const IndividualData &b) const
    {
        return m_compare->compare(a.m_criteria, b.m_criteria);
    }
};

/**
 * Abstract interface for filtering (aka searching) FolksIndividual
 * instances.
 */
class IndividualFilter
{
    int m_maxResults;

 public:
    IndividualFilter() : m_maxResults(-1) {}
    virtual ~IndividualFilter() {}

    /** Maximum number of results. -1 for unlimited. */
    int getMaxResults() const { return m_maxResults; }
    void setMaxResults(int maxResults) { m_maxResults = maxResults; }

    /**
     * True if within the number of expected results.
     */
    bool isIncluded(size_t index) const { return m_maxResults == -1 || index < (size_t)m_maxResults; }

    /**
     * The corresponding EBook query string
     * (http://developer.gnome.org/libebook/stable/libebook-e-book-query.html#e-book-query-to-string)
     * for the filter, if there is one. Empty if not.
     */
    virtual std::string getEBookFilter() const { return ""; }

    /** true if the contact matches the filter */
    virtual bool matches(const IndividualData &data) const = 0;
};

/**
 * A filter which just enforces a maximum number of results,
 * something that FullView cannot do.
 */
class MatchAll : public IndividualFilter
{
 public:
    virtual bool matches(const IndividualData &data) const { return true; }
};

/**
 * A fake filter which just carries the maximum result parameter.
 * Separate type because the dynamic_cast<> can be used to detect
 * this special case.
 */
class ParamFilter : public MatchAll
{
};

class FullView;

/**
 * Is a normal FolksIndividualAggregator and adds sorting, searching
 * and browsing to it. At least the full sorted view always exists.
 */
class IndividualAggregator
{
    /** empty when not started yet */
    boost::shared_ptr<FullView> m_view;
    boost::shared_ptr<IndividualCompare> m_compare;
    boost::shared_ptr<LocaleFactory> m_locale;
    boost::weak_ptr<IndividualAggregator> m_self;
    FolksIndividualAggregatorCXX m_folks;
    FolksBackendStoreCXX m_backendStore;
    /**
     * NULL when backends haven't been loaded yet.
     * Set by backendsLoaded().
     */
    FolksBackendCXX m_eds;
    /**
     * Set by backendsLoaded(), if possible. If m_eds != NULL
     * and m_systemStore == NULL, no system address book is
     * available. If m_eds == NULL, hook into m_backendsLoadedSignal
     * to be notified.
     */
    FolksPersonaStoreCXX m_systemStore;

    boost::signals2::signal<void()> m_backendsLoadedSignal;

    /**
     * The set of enabled EDS databases, referenced by the UUID.
     * Empty by default.
     */
    GeeHashSetCXX m_databases;
    /** string representation for debugging */
    std::string dumpDatabases();

    IndividualAggregator(const boost::shared_ptr<LocaleFactory> &locale);
    void init(boost::shared_ptr<IndividualAggregator> &self);

    /**
     * Called when backend store is prepared. At that point, backends
     * can be disabled or enabled and loading them can be kicked of.
     */
    void storePrepared();

    /**
     * Called when all Folks backends are loaded, before the
     * aggregator does its work. Now is the right time to initialize
     * the set of databases and prepare the aggregator, if start() was
     * already called.
     */
    void backendsLoaded();

    /**
     * Executes the given operation when the EDS system address book
     * is prepared. The operation may throw an exception, which (like
     * all other errors) is reported as failure for the asynchronous
     * operation.
     */
    void runWithAddressBook(const boost::function<void ()> &operation,
                            const ErrorCb_t &onError) throw();
    void runWithAddressBookHaveEDS(const boost::signals2::connection &conn,
                                   const boost::function<void ()> &operation,
                                   const ErrorCb_t &onError) throw();
    void runWithAddressBookPrepared(const GError *gerror,
                                    const boost::function<void ()> &operation,
                                    const ErrorCb_t &onError) throw();

    /**
     * Executes the given operation after looking up the FolksPersona
     * in the system address book, which must be prepared and loaded
     * at that point.
     */
    void runWithPersona(const boost::function<void (FolksPersona *)> &operation,
                        const std::string &localID,
                        const ErrorCb_t &onError) throw();
    void doRunWithPersona(const boost::function<void (FolksPersona *)> &operation,
                          const std::string &localID,
                          const ErrorCb_t &onError) throw();

    /** the operation for runWithAddressBook() */
    void doAddContact(const Result<void (const std::string &)> &result,
                      const PersonaDetails &details);
    /** handle result of adding contact */
    void addContactDone(const GError *gerror,
                        FolksPersona *persona,
                        const Result<void (const std::string &)> &result) throw();

   void doModifyContact(const Result<void ()> &result,
                        FolksPersona *persona,
                        const PersonaDetails &details) throw();
   void doRemoveContact(const Result<void ()> &result,
                        FolksPersona *persona) throw();
   void removeContactDone(const GError *gerror,
                          const Result<void ()> &result) throw();

 public:
    /**
     * Creates an idle IndividualAggregator. Configure it and
     * subscribe to signals, then call start().
     */
    static boost::shared_ptr<IndividualAggregator> create(const boost::shared_ptr<LocaleFactory> &locale);

    /**
     * Access to FolksIndividualAggregator which is owned by
     * the aggregator.
     */
    FolksIndividualAggregator *getFolks() const { return m_folks.get(); }

    /**
     * Set sorting without starting the view just yet.
     */
    void setCompare(const boost::shared_ptr<IndividualCompare> &compare);

    /**
     * Change current locale. Must be followed by setCompare() to update
     * any pre-computed data.
     */
    void setLocale(const boost::shared_ptr<LocaleFactory> &locale);

    /**
     * Starts pulling and sorting of contacts.
     * Creates m_view and starts populating it.
     * Can be called multiple times.
     *
     * See also org.01.pim.contacts.Manager.Start().
     */
    void start();

    /**
     * start() was called, or something caused it to be called.
     */
    bool isRunning() const;

    /**
     * configure active databases
     *
     * @param set of EDS database UUIDs, empty string for the default
     * system address book
     */
    void setDatabases(std::set<std::string> &databases);

    /**
     * Each aggregator has exactly one full view on the data. This
     * method grants access to it and its change signals.
     *
     * @return never empty, start() will be called if necessary
     */
    boost::shared_ptr<FullView> getMainView();

    /**
     * Add contact to system address book. Returns new local ID
     * as result.
     */
   void addContact(const Result<void (const std::string &)> &result,
                   const PersonaDetails &details);

   /**
    * Modify contact in system address book.
    */
   void modifyContact(const Result<void ()> &result,
                      const std::string &localID,
                      const PersonaDetails &details);

   /**
    * Remove contact in system address book.
    */
   void removeContact(const Result<void ()> &result,
                      const std::string &localID);
};


SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_IVI_FOLKS
