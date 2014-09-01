/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
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
#ifndef INCL_SYNC_EVOLUTION_CONFIG
# define INCL_SYNC_EVOLUTION_CONFIG

#include <syncevo/FilterConfigNode.h>
#include <syncevo/SafeConfigNode.h>

#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/foreach.hpp>
#include <list>
#include <string>
#include <sstream>
#include <set>

#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class FileConfigTree;

/**
 * @defgroup ConfigHandling Configuration Handling
 * @{
 */

/**
 * The SyncEvolution configuration is versioned, so that incompatible
 * changes to the on-disk config and files can be made more reliably.
 *
 * The on-disk configuration is versioned at three levels:
 * - root level
 * - context
 * - peer
 *
 * This granularity allows migrating individual peers, contexts or
 * everything to a new format.
 *
 * For each of these levels, two numbers are stored on disk and
 * hard-coded in the binary:
 * - current version = incremented each time the format is extended
 * - minimum version = set to current version each time a backwards
 *                     incompatible change is made
 *
 * This mirrors the libtool library versioning.
 *
 * Reading must check that the on-disk minimum version is <= the
 * binary's current version. Otherwise the config is too recent to
 * be used.
 *
 * Writing will bump minimum and current version on disk to the
 * versions in the binary. It will never decrease versions. This
 * works when the more recent format adds information that can
 * be safely ignored by older releases. If that is not possible,
 * then the "minimum" version must be increased to prevent older
 * releases from using the config.
 *
 * If bumping the versions increases the minimum version
 * beyond the version supported by the release which wrote the config,
 * that release will no longer work. Experimental releases will throw
 * an error and users must explicitly migrate to the current
 * format. Stable releases will migrate automatically.
 *
 * The on-disks current version can be checked to determine how to
 * handle it. It may be more obvious to simple check for the existence
 * of certain properties (that's how this was handled before the
 * introduction of versioning).
 *
 * Here are some simple rules for handling the versions:
 * - increase CUR version when adding new properties or files
 * - set MIN to CUR when it is not safe that older releases
 *   read and write a config with the current format
 *
 * SyncEvolution < 1.2 had no versioning. It's format is 0.
 * SyncEvolution 1.2:
 * - config peer min/cur version 1, because
 *   of modified libsynthesis binfiles and
 * - context min/cur version 1, because
 *   evolutionsource->database, evolutionuser/password->databaseUser/Password
 */
static const int CONFIG_ROOT_MIN_VERSION = 0;
static const int CONFIG_ROOT_CUR_VERSION = 0;
static const int CONFIG_CONTEXT_MIN_VERSION = 1;
static const int CONFIG_CONTEXT_CUR_VERSION = 1;
static const int CONFIG_PEER_MIN_VERSION = 1;
static const int CONFIG_PEER_CUR_VERSION = 1;

enum ConfigLevel {
    CONFIG_LEVEL_ROOT,      /**< = GLOBAL_SHARING */
    CONFIG_LEVEL_CONTEXT,   /**< = SOURCE_SET_SHARING */
    CONFIG_LEVEL_PEER,      /**< = NO_SHARING */
    CONFIG_LEVEL_MAX
};

std::string ConfigLevel2String(ConfigLevel level);

enum ConfigLimit {
    CONFIG_MIN_VERSION,
    CONFIG_CUR_VERSION,
    CONFIG_VERSION_MAX
};

extern int ConfigVersions[CONFIG_LEVEL_MAX][CONFIG_VERSION_MAX];

class SyncSourceConfig;
typedef SyncSourceConfig PersistentSyncSourceConfig;
class ConfigTree;
class UserInterface;
class SyncSourceNodes;
class ConstSyncSourceNodes;
class SyncConfig;
struct ConfigPasswordKey;

/** name of the per-source admin data property */
extern const char *const SourceAdminDataName;

/** name of the sync property for set/getMaxMsgSize */
extern const char *const SyncMaxMsgSize;

struct UserIdentity {
    /**
     * Defines how information about the user and the
     * corresponding authentication method is stored.
     *
     * The config level itself handles USER_IDENTITY_PLAIN_TEXT (=
     * "user", the traditional username/password authentication
     * with the password being stored in the config, in a keyring
     * or retrieved at runtime) and USER_IDENTITY_SYNC_CONFIG (=
     * "id", an indirect lookup mechanism which uses the
     * username/password from a different sync config).
     *
     * Other string values are also allowed in the config. When
     * needed, one of the plugable identity providers must
     * recognize the string and provide credentials.
     *
     * Never empty. However, it may be marked as "unset", in which
     * case the actual string in the config didn't have a prefix
     * and the traditional USER_IDENTITY_PLAIN_TEXT is used as
     * fallback.
     */
    InitStateString m_provider;

    /**
     * The actual username (USER_IDENTITY_PLAIN_TEXT) or config name
     * (USER_IDENTITY_SYNC_CONFIG).
     *
     * It is unset if and only if the config had no value for the "username"
     * property.
     */
    InitStateString m_identity;

    /**
     * Returns concatenation of provider prefix (only if set) and idendity.
     */
    InitStateString toString() const;

    /**
     * Splits the string as generated by toString().
     */
    static UserIdentity fromString(const InitStateString &idString);

    /** unset, empty identity */
    UserIdentity();

    /**
     * true iff the config had a value.
     */
    bool wasSet() const { return m_identity.wasSet(); }
};

/** simplified creation of string lists: InitList("foo") + "bar" + ... */
template<class T> class InitList : public std::list<T> {
 public:
    InitList() {}
    InitList(const T &initialValue) {
        std::list<T>::push_back(initialValue);
    }
    InitList &operator + (const T &rhs) {
        std::list<T>::push_back(rhs);
        return *this;
    }
    InitList &operator += (const T &rhs) {
        std::list<T>::push_back(rhs);
        return *this;
    }
};
typedef InitList<std::string> Aliases;
typedef InitList<Aliases> Values;

enum PropertyType {
    /** sync properties occur once per config */
    SYNC_PROPERTY_TYPE,
    /** source properties occur once per source in each config */
    SOURCE_PROPERTY_TYPE,
    /** exact type is unknown */
    UNKNOWN_PROPERTY_TYPE
};

/**
 * A property name with optional source and context.
 * String format is [<source>/]<property>[@<context>|@<peer>@<context>]
 *
 * Note that the part after the @ sign without another @ is always
 * a context. The normal shorthand of just <peer> without context
 * does not work here.
 */
class PropertySpecifier {
 public:
    std::string m_source;    /**< source name, empty if applicable to all or sync property */
    std::string m_property;  /**< property name, must not be empty */
    std::string m_config;    /**< config name, empty if none, otherwise @<context> or <peer>@<context> */

    enum {
        NO_NORMALIZATION = 0,
        NORMALIZE_SOURCE = 1,
        NORMALIZE_CONFIG = 2
    };

    /** parse, optionally also normalize source and config */
    static PropertySpecifier StringToPropSpec(const std::string &spec, int flags = NORMALIZE_SOURCE|NORMALIZE_CONFIG);
    std::string toString();
};

/**
 * A property has a name and a comment. Derived classes might have
 * additional code to read and write the property from/to a
 * ConfigNode. They might also one or more  of the properties
 * on the fly, therefore the virtual get methods which return a
 * string value and not just a reference.
 *
 * In addition to the name, it may also have aliases. When reading
 * from a ConfigNode, all specified names are checked in the order in
 * which they are listed, and the first one found is used. When
 * writing, an existing key is overwritten, otherwise the main name is
 * created as a new key.
 *
 * A default value is returned if the ConfigNode doesn't have
 * a value set (= empty string). Invalid values in the configuration
 * trigger an exception. Setting invalid values does not because
 * it is not known where the value comes from - the caller should
 * check it himself.
 *
 * Properties are either registered as source properties or
 * source-independent sync properties. In each of these two sets of
 * properties, the names must be unique.
 *
 * Properties can be either user-visible (and editable) or
 * internal. Internal properties are used to cache some information
 * and may get lost when copying configurations. Therefore it
 * must be possible to recreate them somehow, if necessary with
 * an expensive operation like a slow sync.
 *
 * Starting with SyncEvolution 1.0, the internal storage of
 * property values was reorganized so that some properties
 * can be shared between peers. The concept of two property
 * sets (source and sync properties) was preserved because of
 * the simplicity that it brings to the APIs. Now this concept
 * is implemented by mapping properties into "views" that
 * contain the properties relevant for a particular peer.
 * Setting a shared value in one view updates the value in
 * another. For details, see:
 * http://syncevolution.org/development/configuration-handling
 *
 * As in the migration from the Sync4j config layout (internal and
 * user-visible properties in the same file), the code is written so
 * that properties are mapped to config nodes according to the most
 * recent layout. Older layouts are accessed by using the same config
 * node instance multiple times.
 */
class ConfigProperty {
 public:
        ConfigProperty(const std::string &name, const std::string &comment,
                       const std::string &def = std::string(""), const std::string &descr = std::string("")) :
        m_obligatory(false),
        m_hidden(false),
        m_sharing(NO_SHARING),
        m_names(name),
        m_comment(boost::trim_right_copy(comment)),
        m_defValue(def),
        m_descr(descr)
        {}

    ConfigProperty(const Aliases &names, const std::string &comment,
                   const std::string &def = std::string(""), const std::string &descr = std::string("")) :
        m_obligatory(false),
        m_hidden(false),
        m_sharing(NO_SHARING),
        m_names(names),
        m_comment(boost::trim_right_copy(comment)),
            m_defValue(def),
        m_descr(descr)
        {}
    virtual ~ConfigProperty() {}

    /** name to be used for a specific node: first name if not in node, otherwise existing key */
    std::string getName(const ConfigNode &node) const;

    /** primary name */
    std::string getMainName() const { return m_names.front(); }

    /* virtual so that derived classes like SourceBackendConfigProperty can generate the result dynamically */
    virtual const Aliases &getNames() const { return m_names; }
    virtual std::string getComment() const { return m_comment; }
    virtual std::string getDefValue() const { return m_defValue; }
    virtual std::string getDescr() const { return m_descr; }

    /**
     * Check whether the given value is okay.
     * If not, then set an error string (one line, no punctuation).
     *
     * @return true if value is okay
     */
    virtual bool checkValue(const std::string &value, std::string &error) const { return true; }

    /**
     * Only useful when a config property wants to check itself whether to retrieve password
     * Check the password and cache the result in the filter node on the fly if a property needs.
     * sourceName and sourceConfigNode might be not set by caller. They only affect
     * when checking password for syncsourceconfig 
     * @param ui user interface
     * @param config    the peer config in which we are checking the password
     * @param flags     see PasswordConfigProperty
     * @param sourceName   the source name for which we need the password, empty if checking a peer password
     */
    virtual void checkPassword(UserInterface &ui,
                               SyncConfig &config,
                               int flags,
                               const std::string &sourceName = std::string()) const {}

    /**
     * Try to save password if a config property wants.
     * It firstly check password and then invoke ui's savePassword
     * function to save the password if necessary
     */
    virtual void savePassword(UserInterface &ui,
                              SyncConfig &config,
                              const std::string &sourceName = std::string()) const {}

    /**
     * This is used to generate description dynamically according to the context information
     * Default implmenentation is to return value set in the constructor, otherwise the server name.
     * Derived classes can override this function. Used by 'checkPassword' and 'savePassword'
     * to generate description for user interface.
     */
    virtual const std::string getDescr(const std::string &serverName,
                                  FilterConfigNode &globalConfigNode,
                                  const std::string &sourceName = std::string(),
                                  const boost::shared_ptr<FilterConfigNode> &sourceConfigNode=boost::shared_ptr<FilterConfigNode>()) const { return m_descr.empty() ? serverName : m_descr; }

    /** split \n separated comment into lines without \n, appending them to commentLines */
    static void splitComment(const std::string &comment, std::list<std::string> &commentLines);

    /** internal property? */
    bool isHidden() const { return m_hidden; }
    void setHidden(bool hidden) { m_hidden = hidden; }

    /** config is invalid without setting this property? */
    bool isObligatory() const { return m_obligatory; }
    void setObligatory(bool obligatory) { m_obligatory = obligatory; }

    /**
     * determines how a property is shared between different views
     */
    enum Sharing {
        GLOBAL_SHARING,         /**< shared between all views,
                                   for example the "default peer" property */
        SOURCE_SET_SHARING,     /**< shared between all peers accessing
                                   the same source set, for example the
                                   logdir property */
        NO_SHARING              /**< each peer has his own values */
    };
    /** "global", "shared", "unshared" */
    static std::string sharing2str(Sharing sharing);
    Sharing getSharing() const { return m_sharing; }
    void setSharing(Sharing sharing) { m_sharing = sharing; }

    /** set value unconditionally, even if it is not valid */
    void setProperty(ConfigNode &node, const InitStateString &value) const {
        node.setProperty(getName(node),
                         value,
                         getComment());
    }
    void setProperty(ConfigNode &node, const std::string &value) const {
        setProperty(node, InitStateString(value, true));
    }
    void setProperty(FilterConfigNode &node, const InitStateString &value, bool temporarily = false) const {
        std::string name = getName(node);
        if (temporarily) {
            node.addFilter(name, value);
        } else {
            node.setProperty(name, value, getComment());
        }
    }
    void setProperty(FilterConfigNode &node, const std::string &value, bool temporarily = false) const {
        setProperty(node, InitStateString(value, true), temporarily);
    }

    /** set default value of a property, marked as unset unless "force" is true */
    void setDefaultProperty(ConfigNode &node, bool force) const {
        setProperty(node, InitStateString(m_defValue, force));
    }

    /**
     * String representation of property value. getPropertyValue() in
     * some derived classes returns the value in some other, class specific
     * representation.
     */
    virtual InitStateString getProperty(const ConfigNode &node) const {
        std::string name = getName(node);
        InitStateString value = node.readProperty(name);
        if (value.wasSet()) {
            std::string error;
            if (!checkValue(value, error)) {
                throwValueError(node, name, value, error);
            }
            return value;
        } else {
            // default
            return InitStateString(getDefValue(), false);
        }
    }

 protected:
    void throwValueError(const ConfigNode &node, const std::string &name, const std::string &value, const std::string &error) const;

 private:
    bool m_obligatory;
    bool m_hidden;
    Sharing m_sharing;
    const Aliases m_names;
    const std::string m_comment, m_defValue, m_descr;
};

/**
 * A string property which maps multiple different possible value
 * strings to one generic value, ignoring the case. Values not listed
 * are passed through unchanged. The first value in the list of
 * aliases is the generic one.
 *
 * The addition operator is defined for the aliases so that they
 * can be constructed more easily.
 */
class StringConfigProperty : public ConfigProperty {
 public:
    StringConfigProperty(const std::string &name, const std::string &comment,
                         const std::string &def = std::string(""),
                         const std::string &descr = std::string(""),
                         const Values &values = Values()) :
    ConfigProperty(name, comment, def, descr),
        m_values(values)
        {}

    /**
     * @return false if aliases are defined and the string is not one of them
     */
    bool normalizeValue(std::string &res) const {
        Values values = getValues();
        BOOST_FOREACH(const Values::value_type &value, values) {
            BOOST_FOREACH(const std::string &alias, value) {
                if (boost::iequals(res, alias)) {
                    res = *value.begin();
                    return true;
                }
            }
        }
        return values.empty();
    }

    /**
     * This implementation accepts all values if no aliases
     * are given, otherwise the value must be part of the aliases.
     */
    virtual bool checkValue(const std::string &propValue, std::string &error) const {
        Values values = getValues();
        if (values.empty()) {
            return true;
        }

        std::ostringstream err;
        err << "not one of the valid values (";
        bool firstval = true;
        BOOST_FOREACH(const Values::value_type &value, values) {
            if (!firstval) {
                err << ", ";
            } else {
                firstval = false;
            }
            bool firstalias = true;
            BOOST_FOREACH(const std::string &alias, value) {
                if (!firstalias) {
                    err << " = ";
                } else {
                    firstalias = false;
                }
                if (alias.empty()) {
                    err << "\"\"";
                } else {
                    err << alias;
                }
                
                if (boost::iequals(propValue, alias)) {
                    return true;
                }
            }
        }
        err << ")";
        error = err.str();
        return false;
    }

    virtual InitStateString getProperty(const ConfigNode &node) const {
        InitStateString res = ConfigProperty::getProperty(node);
        normalizeValue(res);
        return res;
    }

 protected:
    virtual Values getValues() const { return m_values; }

 private:
    const Values m_values;
};


/**
 * Instead of reading and writing strings, this class interprets the content
 * as a specific type.
 */
template<class T> class TypedConfigProperty : public ConfigProperty {
 public:
    TypedConfigProperty(const std::string &name, const std::string &comment, const std::string &defValue = std::string("0"), const std::string &descr = std::string("")) :
    ConfigProperty(name, comment, defValue, descr)
        {}

    /**
     * This implementation accepts all values that can be converted
     * to the required type.
     */
    virtual bool checkValue(const std::string &value, std::string &error) const {
        std::istringstream in(value);
        T res;
        if (in >> res) {
            return true;
        } else {
            error = "cannot parse value";
            return false;
        }
    }

    void setProperty(ConfigNode &node, const T &value) const {
        std::ostringstream out;

        out << value;
        node.setProperty(getName(node), out.str(), getComment());
    }
    void setProperty(FilterConfigNode &node, const T &value, bool temporarily = false) const {
        std::ostringstream out;
        std::string name = getName(node);

        out << value;
        InitStateString res(out.str(), true);
        if (temporarily) {
            node.addFilter(name, res);
        } else {
            node.setProperty(name, res, getComment());
        }
    }

    InitState<T> getPropertyValue(const ConfigNode &node) const {
        std::string name = getName(node);
        std::string value = node.readProperty(name);
        std::istringstream in(value);
        T res;
        if (value.empty()) {
            // use default
            std::istringstream defStream(getDefValue());
            defStream >> res;
            return InitState<T>(res, false);
        } else {
            // use explicitly set value
            if (!(in >> res)) {
                throwValueError(node, name, value, "cannot parse value");
            }
            return InitState<T>(res, true);
        }
    }
};

/**
 * The "in >> res" check in TypedConfigProperty::checkValue
 * is to loose. For example, the standard library accepts
 * -1 for an unsigned type.
 *
 * This class accepts a function pointer to strtoul() or
 * strtol() and uses that function to do strict value checking.
 *
 * @param T       type to be converted to and from (like int)
 * @param Tmin    minimum value of T for range checking
 * @param Tmax    maximum value of T for range checking
 * @param C       intermediate type for conversion  (like long)
 * @param Cmin    minimum value of C for range checking
 * @param Cmax    maximum value of C for range checking
 * @param strto   conversion function
 */
template <class T, T Tmin, T Tmax,
    class C, C Cmin, C Cmax,
    C (*strto)(const char *, char **, int)>
class ScalarConfigProperty : public TypedConfigProperty<T>
{
 public:
    ScalarConfigProperty(const std::string &name, const std::string &comment, const std::string &defValue = std::string("0"), const std::string &descr = std::string("")) :
    TypedConfigProperty<T>(name, comment, defValue, descr)
        {}

    virtual bool checkValue(const std::string &value, std::string &error) const {
        errno = 0;
        const char *nptr = value.c_str();
        char *endptr;
        // use base 10 because not specifying a base 
        // would interpret 077 as octal value, which
        // could be confusing for users
        C val = strto(nptr, &endptr, 10);
        if ((errno == ERANGE && (val == Cmin || val == Cmax))) {
            error = "range error";
            return false;
        }
        if (errno != 0 && val == 0) {
            error = "cannot parse";
            return false;
        }
        if (endptr == nptr) {
            error = "decimal value expected";
            return false;
        }
        while (isspace(*endptr)) {
            endptr++;
        }
        if (*endptr != '\0') {
            error = "unexpected trailing non-whitespace characters: ";
            error += endptr;
            return false;
        }
        // comparison might be always true for some types
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-compare"
#endif
        if (val > Tmax || val < Tmin) {
            error = "range error";
            return false;
        }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
        if (Tmin == 0) {
            // check that we didn't accidentally accept a negative value,
            // strtoul() does that
            const char *start = nptr;
            while (*start && isspace(*start)) {
                start++;
            }
            if (*start == '-') {
                error = "range error";
                return false;
            }
        }

        return true;
    }
};

typedef ScalarConfigProperty<int, INT_MIN, INT_MAX, long, LONG_MIN, LONG_MAX, strtol> IntConfigProperty;
typedef ScalarConfigProperty<unsigned int, 0, UINT_MAX, unsigned long, 0, ULONG_MAX, strtoul> UIntConfigProperty;
typedef ScalarConfigProperty<long, LONG_MIN, LONG_MAX, long, LONG_MIN, LONG_MAX, strtol> LongConfigProperty;
typedef ScalarConfigProperty<unsigned long, 0, ULONG_MAX, unsigned long, 0, ULONG_MAX, strtoul> ULongConfigProperty;

/**
 * Time interval >= 0. Values are formatted as number of seconds
 * and accepted in a variety of formats, following ISO 8601:
 * - x = x seconds
 * - d = x[YyWwDdHhMmSs] = x years/weeks/days/hours/minutes/seconds
 * - d[+]d... = combination of the previous durations
 *
 * As an extension of ISO 8601, white spaces are silently ignored,
 * suffix checks are case-insensitive and s (or S) for seconds
 * can be omitted.
 */
class SecondsConfigProperty : public UIntConfigProperty
{
 public:
    SecondsConfigProperty(const std::string &name, const std::string &comment,
                          const std::string &defValue = std::string("0"), const std::string &descr = "") :
        UIntConfigProperty(name, comment, defValue, descr)
        {}

    virtual bool checkValue(const std::string &value, std::string &error) const;
    InitState<unsigned int> getPropertyValue(const ConfigNode &node) const;

    static bool parseDuration(const std::string &value, std::string &error, unsigned int &seconds);
};

/**
 * Supports lookup of password via env variable, interactively, and
 * keyring. The keys for the keyring are derived from other properties
 * in derived classes.
 */
class PasswordConfigProperty : public ConfigProperty {
 public:
    PasswordConfigProperty(const std::string &name, const std::string &comment, const std::string &def = std::string(""),const std::string &descr = std::string("")) :
       ConfigProperty(name, comment, def, descr)
           {}
    PasswordConfigProperty(const Aliases &names, const std::string &comment, const std::string &def = std::string(""),const std::string &descr = std::string("")) :
       ConfigProperty(names, comment, def, descr)
           {}

    /**
     * Check the password and cache the result. Accessed via base
     * ConfigProperty class, must be implemented in derived classes.
     */
    virtual void checkPassword(UserInterface &ui,
                               SyncConfig &config,
                               int flags,
                               const std::string &sourceName = std::string()) const = 0;

    /**
     * Move password to storage. Accessed via base ConfigProperty
     * class, must be implemented in derived classes.
     */
    virtual void savePassword(UserInterface &ui,
                              SyncConfig &config,
                              const std::string &sourceName = std::string()) const = 0;

    /**
     * Utility method, invokes checkPassword() on the properties selected
     * via flags and sourceNames array. Optionally updates the properties
     * to reflect the actual values which need to be used.
     */
    static void checkPasswords(UserInterface &ui,
                               SyncConfig &config,
                               int flags,
                               const std::list<std::string> &sourceNames = std::list<std::string>());

    enum {
        CHECK_PASSWORD_RESOLVE_USERNAME = 1<<0,   ///< temporarily replace username that corresponds to the password
        CHECK_PASSWORD_RESOLVE_PASSWORD = 1<<1,   ///< temporarily replace password with actual value
        CHECK_PASSWORD_SYNC = 1<<2,               ///< check sync properties
        CHECK_PASSWORD_SOURCE = 1<<3,             ///< check source properties

        /** Alias for the "check and resolve" everything. */
        CHECK_PASSWORD_ALL = CHECK_PASSWORD_RESOLVE_USERNAME|CHECK_PASSWORD_RESOLVE_PASSWORD|CHECK_PASSWORD_SYNC|CHECK_PASSWORD_SOURCE,

        CHECK_PASSWORD_FLAG_NONE = 0
    };

 protected:

    /**
     * The actual implementation, with additional information
     * provided by derived classes.
     */
    void checkPassword(UserInterface &ui,
                       SyncConfig &config,
                       int flags,
                       const ConfigProperty &usernameProperty,
                       const std::string &sourceName = std::string()) const;

    void savePassword(UserInterface &ui,
                      SyncConfig &config,
                      const ConfigProperty &usernameProperty,
                      const std::string &sourceName = std::string()) const;

    /**
     * Get password lookup key for storing or retrieving passwords.
     *
     * @param descr decription for password
     * @param globalConfigNode the global config node 
     * @param sourceConfigNode the source config node. It might be empty
     */
    virtual ConfigPasswordKey getPasswordKey(const std::string &descr,
                                             const std::string &serverName,
                                             FilterConfigNode &globalConfigNode,
                                             const std::string &sourceName = std::string(),
                                             const boost::shared_ptr<FilterConfigNode> &sourceConfigNode =
                                             boost::shared_ptr<FilterConfigNode>()) const = 0;

    static std::string getUsername(const ConfigProperty &usernameProperty,
                                   const FilterConfigNode &node);
};

/**
 * Instead of reading and writing strings, this class interprets the content
 * as boolean with T/F or 1/0 (default format).
 */
class BoolConfigProperty : public StringConfigProperty {
 public:
    BoolConfigProperty(const std::string &name, const std::string &comment, const std::string &defValue = std::string("F"),const std::string &descr = std::string("")) :
    StringConfigProperty(name, comment, defValue,descr,
                         Values() + (Aliases("1") + "T" + "TRUE") + (Aliases("0") + "F" + "FALSE"))
        {}

    void setProperty(ConfigNode &node, bool value) {
        StringConfigProperty::setProperty(node, InitStateString(value ? "1" : "0", true));
    }
    void setProperty(FilterConfigNode &node, bool value, bool temporarily = false) {
        StringConfigProperty::setProperty(node, InitStateString(value ? "1" : "0", true), temporarily);
    }
    InitState<bool> getPropertyValue(const ConfigNode &node) const {
        InitStateString res = ConfigProperty::getProperty(node);

        return InitState<bool>(boost::iequals(res, "T") ||
                               boost::iequals(res, "TRUE") ||
                               atoi(res.c_str()) != 0,
                               res.wasSet());
    }
};

/**
 * A property for arbitrary strings.
 */
class SafeConfigProperty : public ConfigProperty {
 public:
    SafeConfigProperty(const std::string &name, const std::string &comment) :
    ConfigProperty(name, comment)
    {}

    void setProperty(ConfigNode &node, const InitStateString &value) {
        ConfigProperty::setProperty(node,
                                    InitStateString(StringEscape::escape(value, '!', StringEscape::INI_WORD),
                                                    value.wasSet()));
    }
    void setProperty(ConfigNode &node, const std::string &value) {
        ConfigProperty::setProperty(node,
                                    InitStateString(StringEscape::escape(value, '!', StringEscape::INI_WORD),
                                                    true));
    }
    virtual InitStateString getProperty(const ConfigNode &node) const {
        InitStateString res = ConfigProperty::getProperty(node);
        return InitStateString(StringEscape::unescape(res, '!'),
                               res.wasSet());
    }
};

/**
 * A registry for all properties which might be saved in the same ConfigNode.
 * Currently the same as a simple list. Someone else owns the instances.
 */
class ConfigPropertyRegistry : public std::list<const ConfigProperty *> {
 public:
    /** case-insensitive search for property */
    const ConfigProperty *find(const std::string &propName) const {
        BOOST_FOREACH(const ConfigProperty *prop, *this) {
            BOOST_FOREACH(const std::string &name, prop->getNames()) {
                if (boost::iequals(name, propName)) {
                    return prop;
                }
            }
        }
        return NULL;
    }
};

/**
 * This class implements the client library configuration interface
 * by mapping values to properties to entries in a ConfigTree. The
 * mapping is either the traditional one used by SyncEvolution <= 0.7
 * and client library <= 6.5 or the new layout introduced with
 * SyncEvolution >= 0.8. If for a given server name the old config
 * exists, then it is used. Otherwise the new layout is used.
 *
 * This class can be instantiated on its own and then provides access
 * to properties actually stored in files. SyncContext
 * inherits from this class so that a derived client has the chance to
 * override every single property (although it doesn't have to).
 * Likewise SyncSource is derived from
 * SyncSourceConfig.
 *
 * Properties can be set permanently (this changes the underlying
 * ConfigNode) and temporarily (this modifies the FilterConfigNode
 * which wraps the ConfigNode).
 *
 * The old layout is:
 * - $HOME/.sync4j/evolution/<server>/spds/syncml/config.txt
 * -- spds/sources/<source>/config.txt
 * ---                      changes_<changeid>/config.txt
 *
 * The new layout is:
 * - ${XDG_CONFIG:-${HOME}/.config}/syncevolution/foo - base directory for server foo
 * -- config.ini - constant per-server settings
 * -- .internal.ini - read/write server properties - hidden from users because of the leading dot
 * -- sources/bar - base directory for source bar
 * --- config.ini - constant per-source settings
 * --- .internal.ini - read/write source properties
 * --- .changes_<changeid>.ini - change tracking node (content under control of sync source)
 *
 * Because this class needs to handle different file layouts it always
 * uses a FileConfigTree instance. Other implementations would be
 * possible.
 */
class SyncConfig {
 public:
    /**
     * Opens the configuration for a specific server,
     * searching for the config files in the usual
     * places. Will succeed even if config does not
     * yet exist: flushing such a config creates it.
     *
     * Does a version check to ensure that the config can be
     * read. Users of the instance must to an explicit
     * prepareConfigForWrite() if the config or the files associated
     * with it (Synthesis bin files) are going to be written.
     *
     * @param peer   string that identifies the peer,
     *               matching regex (.*)(@([^@]*))? 
     *               where the $1 (the first part) is 
     *               the peer name and the optional $2
     *               (the part after the last @) is the
     *               context, "default" if not given.
     *               For example "scheduleworld" =
     *               "ScheduleWorld" =
     *               "scheduleworld@default", but not the same as
     *               "scheduleworld@other_context"
     *
     * @param tree   if non-NULL, then this is used
     *               as configuration tree instead of
     *               searching for it; always uses the
     *               current layout in that tree
     *
     * @param redirectPeerRootPath
     *               Can be used to redirect the per-peer
     *               files into a different directory. Only works
     *               in non-peer context configs.
     *               Used by SyncContext for local sync.
     */
    SyncConfig(const std::string &peer,
               boost::shared_ptr<ConfigTree> tree = boost::shared_ptr<ConfigTree>(),
               const std::string &redirectPeerRootPath = "");


    /**
     * Creates a temporary configuration.
     * Can be copied around, but not flushed.
     */
    SyncConfig();

    virtual ~SyncConfig() {}

    /**
     * Discard all changes made to config files.
     * Makes it impossible to do incremental syncs!
     *
     * Not 100% reliable, but works in practice:
     * - Source nodes for change tracking are replaced
     *   with volatile nodes (reliable).
     * - Flushing nodes is skipped, which covers peer and global
     *   nodes. They need to provide the real content and therefore
     *   cannot be replaced. Could be circumvented by flushing them
     *   directly.
     */
    void makeEphemeral();
    bool isEphemeral() const { return m_ephemeral; }

    /**
     * determines whether the need to migrate a config causes a
     * STATUS_MIGRATION_NEEDED error or does the migration
     * automatically; default is to migrate automatically in
     * stable releases and to ask in development releases
     */
    enum ConfigWriteMode {
        MIGRATE_AUTOMATICALLY,
        ASK_USER_TO_MIGRATE
    };
    ConfigWriteMode getConfigWriteMode() const { return m_configWriteMode; }
    void setConfigWriteMode(ConfigWriteMode mode) { m_configWriteMode = mode; }

    /**
     * This does another version check which ensures that the config
     * is not unintentionally altered so that it cannot be read by
     * older SyncEvolution releases. If the config cannot be written
     * without breaking older releases, then either the call will fail
     * (development releases) or migrate the config (stable releases).
     * Can be controlled via setConfigWriteMode();
     *
     * Also writes the current config versions into the config.
     */
    void prepareConfigForWrite();

   /** absolute directory name of the configuration root, empty if unknown or not available */
    std::string getRootPath() const;

    typedef std::list< std::pair<std::string, std::string> > ConfigList;

    /** A simple description of the template or the configuration based on a
     * template. The rank field is used to indicate how good it matches the
     * user input <MacAddress, DeviceName> */
    struct TemplateDescription {
        // The unique identifier of the template
        std::string m_templateId;
        // The description of the template (eg. the web server URL for a
        // SyncML server. This is not used for UI, only CMD line used this.
        std::string m_description;
        // The matched percentage of the template, larger the better.
        int m_rank;

        // This can be either the user-modifiable device name, vendor
        // name, or product name (vendor + model). This depends on
        // whether the device supports the Bluetooth Device ID profile
        // and, if so, whether we have the model in the lookup table.
        std::string m_hardwareName;

        //a unique identity of the device that the template is for, used by caller
        std::string m_deviceId;

        // This is always the user-modifiable device name.
        std::string m_deviceName;

        // A unique string identify the template path, so that a later operation
        // fetching this config will be much easier
        std::string m_path;

        // A string indicates the original fingerprint in the matched template, this
        // will not necessarily be the same as m_deviceName
        std::string m_matchedModel;

        // The template name (device class) presented
        std::string m_templateName;

        TemplateDescription (const std::string &templateId, const std::string &description,
                             const int rank, const std::string &hardwareName, const std::string &deviceId,
                             const std::string &fingerprint, const std::string &path,
                             const std::string &model, const std::string &templateName)
            :   m_templateId (templateId),
                m_description (description),
                m_rank (rank),
                m_hardwareName (hardwareName),
                m_deviceId (deviceId),
                m_deviceName (fingerprint),
                m_path (path),
                m_matchedModel(model),
                m_templateName (templateName)
        {
        }

        TemplateDescription (const std::string &name, const std::string &description);

        static bool compare_op (boost::shared_ptr<TemplateDescription> &left, boost::shared_ptr<TemplateDescription> &right);
    };

    enum MatchMode {
        /*Match templates when we work as SyncML server, i.e. the peer is the client*/
        MATCH_FOR_SERVER_MODE,
        /*Match templates when work as SyncML client, i.e. the peer is the server*/
        MATCH_FOR_CLIENT_MODE,
        /*Match templates for both SyncML server and SyncML client*/
        MATCH_ALL,
        INVALID
    };

    typedef std::list<boost::shared_ptr <TemplateDescription> > TemplateList;

    /* This information is available if the device supports the
     * Device Id Profile.
     */
    struct PnpInformation
    {
        const std::string m_vendor;
        const std::string m_product;
        bool isKnownProduct() const {return !m_product.empty();}
        PnpInformation(const std::string &vendor,
                       const std::string &product)
            :m_vendor(vendor), m_product(product)
        {}
    };

    struct DeviceDescription {
        /** the id of the device */
        std::string m_deviceId;
        /* The user-modifiable name of the device. This will be used
         * as the fingerprint if the PnpInformation is not
         * available. */
        std::string m_deviceName;
        /* For bluetooth devices, we use PnpInformation's immutable
         * product id which provides a more reliable fingerprint than
         * the user-modifiable device string. The fingerprint of the
         * device is used for matching templates. */
        std::string getFingerprint() const;
        /** match mode used for matching templates */
        MatchMode m_matchMode;
        /** the PnPInformation for the device if available */
        boost::shared_ptr<PnpInformation> m_pnpInformation;

        DeviceDescription(const std::string &deviceId,
                          const std::string &deviceName,
                          MatchMode mode)
            :m_deviceId(deviceId), m_deviceName(deviceName), m_matchMode(mode)
        {}
        DeviceDescription() : m_matchMode(INVALID)
        {}
    };

    typedef std::list<DeviceDescription> DeviceList;

    /**
     * returns list of servers in either the old (.sync4j) or
     * new config directory (.config), given as server name
     * and absolute root of config
     *
     * Guaranteed to be sorted by the (context, peer name, path) tuple,
     * in increasing order (foo@bar < abc@xyz < abc.old@xyz).
     */
    static ConfigList getConfigs();

    /**
     * TODO: fix description of matchPeerTemplates() and remove getPeerTemplates()
     *
     * returns list of available config templates:
     * for each peer listed in @peers, matching against the fingerprint information
     * from the peer (deviceName likely), sorted by the matching score,
     * templates failed to match(as long as it's for SyncML server) will also
     * be returned as a fallback mechanism so that user can select a configuration
     * template manually.
     * Any templates for SyncMl Client is also returned, with a default rank.
     * The assumption currently is only work for SyncML client peers.
     * DeviceList is a list of matching tuples <fingerprint, SyncConfig::MatchMode>.
     */
    static TemplateList getPeerTemplates(const DeviceList &peers) { return matchPeerTemplates(peers, true); }

    /**
     * match the built-in templates against @param fingerprint, return a list of
     * servers sorted by the matching rank.
     * */
    static TemplateList matchPeerTemplates(const DeviceList &peers, bool fuzzyMatch = true);

    /**
     * Creates a new instance of a configuration template.
     * The result can be modified to set filters, but it
     * cannot be flushed.
     *
     * @param peer   a configuration name, *without* a context (scheduleworld, not scheduleworld@default),
     * or a configuration path in the system directory which can avoid another fuzzy match process.
     * "none" returns an empty template (default sync properties and dev ID set).
     * @return NULL if no such template
     */
    static boost::shared_ptr<SyncConfig> createPeerTemplate(const std::string &peer);

    /**
     * true if the main configuration file already exists;
     * "main" here means the per-peer config or context config,
     * depending on what the config refers to
     */
    bool exists() const;

    /**
     * true if the config files for the selected level exist;
     * false is returned for CONFIG_LEVEL_PEER and a config
     * which refers to a context
     */
    bool exists(ConfigLevel level) const;

    /**
     * The normalized, unique config name used by this instance.
     * Empty if not backed up by a real config.
     */
    std::string getConfigName() const { return m_peer; }

    /**
     * The normalized context used by this instance.
     * Includes @ sign.
     */
    std::string getContextName() const;

    /**
     * the normalized peer name, empty if not a peer config
     */
    std::string getPeerName() const;

    /**
     * true if the config is for a peer, false if a context config
     */
    bool hasPeerProperties() const { return !m_peerPath.empty(); }

    /**
     * returns names of peers inside this config;
     * empty if not a context
     */
    std::list<std::string> getPeers() const;

    /**
     * Do something before doing flush to files. This is particularly
     * useful when user interface wants to do preparation jobs, such
     * as savePassword and others.
     */
    virtual void preFlush(UserInterface &ui); 

    void flush();

    /**
     * Remove the configuration. Config directories are removed if
     * empty.
     *
     * When the configuration is peer-specific, only the peer's
     * properties and config nodes are removed. Otherwise the complete
     * configuration is removed, including all peers.
     *
     * Does *not* remove logs associated with the configuration.
     * For that use the logdir handling in SyncContext
     * before removing the configuration.
     */
    void remove();

    /**
     * A list of all properties. Can be extended by derived clients.
     */
    static ConfigPropertyRegistry &getRegistry();

    enum NormalizeFlags {
        NORMALIZE_LONG_FORMAT = 0,  /**< include context in normal form */
        NORMALIZE_SHORTHAND = 1,    /**< keep normal form shorter by not specifying @default */
        NORMALIZE_IS_NEW = 2,       /**< does not refer to an existing config, do not search
                                       for it among existing configs */
        NORMALIZE_MAX = 0xFFFF
    };

    /**
     * Normalize a config string:
     * - lower case
     * - non-printable and unsafe characters (colon, slash, backslash)
     *   replaced by underscore
     * - when no context specified and NORMALIZE_IS_NEW not set:
     *   search for peer config first in @default, then also in other contexts
     *   in alphabetical order
     * - NORMALIZE_SHORTHAND set: @default stripped  (dangerous: result "foo"
     *   may incorrectly be mapped to "foo@bar" if the "foo@default" config gets removed),
     *   otherwise added if missing
     * - empty string replaced with "@default"
     */
    static std::string normalizeConfigString(const std::string &config, NormalizeFlags flags = NORMALIZE_SHORTHAND);

    /**
     * Split a config string (normalized or not) into the peer part
     * (before final @) and the context (after that @, not including
     * it), return "default" as context if not specified otherwise.
     *
     * @return true when the context was specified explicitly
     */
    static bool splitConfigString(const std::string &config, std::string &peer, std::string &context);

    /**
     * Replaces the property filter of either the sync properties or
     * all sources. This can be used to e.g. temporarily override
     * the active sync mode.
     *
     * All future calls of getSyncSourceNodes() will have these
     * filters applied.
     *
     * @param sync     true if the filter applies to sync properties,
     *                 false if it applies to sources
     * @param source   empty string if filter applies to all sources,
     *                 otherwise the source name to which it applies
     * @param filter   key (case insensitive)/value pairs of properties
     *                 which are to be overridden
     */
    void setConfigFilter(bool sync,
                         const std::string &source,
                         const FilterConfigNode::ConfigFilter &filter);

    /**
     * Read-write access to all configurable properties of the server.
     * The visible properties are passed through the config filter,
     * which can be modified.
     */
    boost::shared_ptr<FilterConfigNode> getProperties(bool hidden = false) { return m_props[hidden]; }
    boost::shared_ptr<const FilterConfigNode> getProperties(bool hidden = false) const { return const_cast<SyncConfig *>(this)->getProperties(hidden); }

    /**
     * Returns the right config node for a certain property,
     * depending on visibility and sharing.
     */
    boost::shared_ptr<FilterConfigNode> getNode(const ConfigProperty &prop);
    boost::shared_ptr<const FilterConfigNode> getNode(const ConfigProperty &prop) const 
    {
        return const_cast<SyncConfig *>(this)->getNode(prop);
    }

    /**
     * Returns the right config node for a certain registered property,
     * looked up by name. NULL if not found.
     */
    boost::shared_ptr<FilterConfigNode> getNode(const std::string &propName);
    boost::shared_ptr<const FilterConfigNode> getNode(const std::string &propName) const
    {
        return const_cast<SyncConfig *>(this)->getNode(propName);
    }

    /**
     * Returns a wrapper around all properties of the given source
     * which are saved in the config tree. Note that this is different
     * from the set of sync source configs used by the SyncManager:
     * the SyncManger uses the AbstractSyncSourceConfig. In
     * SyncEvolution those are implemented by the
     * SyncSource's actually instantiated by
     * SyncContext. Those are complete whereas
     * PersistentSyncSourceConfig only provides access to a
     * subset of the properties.
     *
     * Can be called for sources which do not exist yet.
     */
    virtual boost::shared_ptr<PersistentSyncSourceConfig> getSyncSourceConfig(const std::string &name);
    virtual boost::shared_ptr<const PersistentSyncSourceConfig> getSyncSourceConfig(const std::string &name) const {
        return const_cast<SyncConfig *>(this)->getSyncSourceConfig(name);
    }

    /**
     * Returns list of all configured (not active!) sync sources.
     */
    virtual std::list<std::string> getSyncSources() const;

    /**
     * Creates config nodes for a certain node. The nodes are not
     * yet created in the backend if they do not yet exist.
     *
     * Calling this for the same name repeatedly will return the
     * same set of node instances. This allows to set properties
     * temporarily in one place and have them used elsewhere.
     *
     * setConfigFilter() resets this cache of nodes. Requesting nodes
     * after that call will create a new set of nodes with properties
     * modified temporarily according to these filters.
     *
     * @param name       the name of the sync source
     * @param trackName  additional part of the tracking node name (used for unit testing)
     */
    SyncSourceNodes getSyncSourceNodes(const std::string &name,
                                       const std::string &trackName = "");
    ConstSyncSourceNodes getSyncSourceNodes(const std::string &name,
                                            const std::string &trackName = "") const;

    /**
     * Creates config nodes for a certain node. The nodes are not
     * yet created in the backend if they do not yet exist.
     * In contrast to the normal set of nodes, the tracking node
     * is empty and discards all changes. This is useful when
     * trying to initialize a SyncSource without a peer (normally
     * has a tracking node which rejects writes with an exception)
     * or with a peer without interfering with normal change tracking
     * (normally SyncSource might overwrite change tracking).
     *
     * @param name          the name of the sync source
     */
    SyncSourceNodes getSyncSourceNodesNoTracking(const std::string &name);

    /**
     * initialize all properties with their default value
     */
    void setDefaults(bool force = true);

    /**
     * create a new sync source configuration with default values
     */
    void setSourceDefaults(const std::string &name, bool force = true);

    /**
     * Remove sync source configuration. And remove the directory
     * if it has no other files.
     *
     * When the configuration is peer-specific, only the peer's
     * properties are removed. Otherwise the complete source
     * configuration is removed, including properties stored
     * for in any of the peers.
     */
    void removeSyncSource(const std::string &name);

    /**
     * clear existing visible source properties selected by the
     * configuration: with or without peer-specific properties,
     * depending on the current view
     */
    void clearSyncSourceProperties(const std::string &name);

    /**
     * clear all global sync properties, with or without
     * peer-specific properties, depending on the current view
     */
    void clearSyncProperties();

    /**
     * Copy all registered properties (hidden and visible) and the
     * tracking node into the current config. This is done by reading
     * all properties from the source config, which implies the unset
     * properties will be set to their default values.  The current
     * config is not cleared so additional, unregistered properties
     * (should they exist) will continue to exist unchanged.
     *
     * The current config still needs to be flushed to make the
     * changes permanent.
     *
     * @param sources   if NULL, then copy all sources; if not NULL,
     *                  then copy exactly the sources listed here
     *                  (regardless whether they exist or not)
     */
    void copy(const SyncConfig &other,
              const std::set<std::string> *sources);

    /**
     * @name Settings specific to SyncEvolution
     *
     * See the property definitions in SyncConfig.cpp
     * for the user-visible explanations of
     * these settings.
     */
    /**@{*/

    virtual InitStateString getDefaultPeer() const;
    virtual void setDefaultPeer(const std::string &value);

    virtual InitStateTri getKeyring() const;
    virtual void setKeyring(const std::string &value);

    virtual InitStateString getLogDir() const;
    virtual void setLogDir(const std::string &value, bool temporarily = false);

    virtual InitState<unsigned int> getMaxLogDirs() const;
    virtual void setMaxLogDirs(unsigned int value, bool temporarily = false);

    virtual InitState<unsigned int> getLogLevel() const;
    virtual void setLogLevel(unsigned int value, bool temporarily = false);

    enum NotifyLevel {
        NOTIFY_NONE,        /**< suppress all notifications */
        NOTIFY_ERROR,       /**< show only errors */
        NOTIFY_CHANGES,     /**< show information about changes and errors */
        NOTIFY_ALL          /**< show all notifications, including starting a sync */
    };
    virtual InitState<NotifyLevel> getNotifyLevel() const;
    virtual void setNotifyLevel(NotifyLevel value, bool temporarily = false);

    virtual InitState<bool> getPrintChanges() const;
    virtual void setPrintChanges(bool value, bool temporarily = false);

    virtual InitState<bool> getDumpData() const;
    virtual void setDumpData(bool value, bool temporarily = false);

    virtual InitStateString getWebURL() const;
    virtual void setWebURL(const std::string &url, bool temporarily = false);

    virtual InitStateString getIconURI() const;
    virtual void setIconURI(const std::string &uri, bool temporarily = false);

    /**
     * A property of server template configs. True if the server is
     * ready for use by "normal" users (everyone can get an account
     * and some kind of support, we have tested the server well
     * enough, ...).
     */
    virtual InitState<bool> getConsumerReady() const;
    virtual void setConsumerReady(bool ready);

    virtual InitState<unsigned long> getHashCode() const;
    virtual void setHashCode(unsigned long hashCode);

    virtual InitStateString getConfigDate() const;
    virtual void setConfigDate(); /* set current time always */

    /**@}*/

    /**
     * @name SyncML Settings
     *
     * These settings are required by the Synthesis engine.
     * Some of them are hard-coded in this class. A derived class could
     * make them configurable again, should that be desired.
     */
    /**@{*/

    virtual UserIdentity getSyncUser() const;
    virtual void setSyncUsername(const std::string &value, bool temporarily = false);
    virtual InitStateString getSyncPassword() const;
    virtual void setSyncPassword(const std::string &value, bool temporarily = false);

    virtual InitState<bool> getPreventSlowSync() const;
    virtual void setPreventSlowSync(bool value, bool temporarily = false);
    virtual InitState<bool> getUseProxy() const;
    virtual void setUseProxy(bool value, bool temporarily = false);
    virtual InitStateString getProxyHost() const;
    virtual void setProxyHost(const std::string &value, bool temporarily = false);
    virtual UserIdentity getProxyUser() const;
    virtual void setProxyUsername(const std::string &value, bool temporarily = false);
    virtual InitStateString getProxyPassword() const;
    virtual void setProxyPassword(const std::string &value, bool temporarily = false);
    virtual InitState< std::vector<std::string> > getSyncURL() const;
    virtual void setSyncURL(const std::string &value, bool temporarily = false);
    virtual void setSyncURL(const std::vector<std::string> &value, bool temporarily = false);
    virtual InitStateString getClientAuthType() const;
    virtual void setClientAuthType(const std::string &value, bool temporarily = false);
    virtual InitState<unsigned long> getMaxMsgSize() const;
    virtual void setMaxMsgSize(unsigned long value, bool temporarily = false);
    virtual InitState<unsigned int> getMaxObjSize() const;
    virtual void setMaxObjSize(unsigned int value, bool temporarily = false);
    virtual InitStateString getSSLServerCertificates() const;

    /**
     * iterate over files mentioned in getSSLServerCertificates()
     * and return name of first one which is found, empty string
     * if none
     */
    std::string findSSLServerCertificate();

    virtual void setSSLServerCertificates(const std::string &value, bool temporarily = false);
    virtual InitState<bool> getSSLVerifyServer() const;
    virtual void setSSLVerifyServer(bool value, bool temporarily = false);
    virtual InitState<bool> getSSLVerifyHost() const;
    virtual void setSSLVerifyHost(bool value, bool temporarily = false);
    virtual InitState<unsigned int> getRetryInterval() const;
    virtual void setRetryInterval(unsigned int value, bool temporarily = false);
    virtual InitState<unsigned int> getRetryDuration() const;
    virtual void setRetryDuration(unsigned int value, bool temporarily = false);
    virtual InitStateString getDevID() const;
    virtual void setDevID(const std::string &value, bool temporarily = false);

    /*Used for Server Alerted Sync*/
    virtual InitStateString getRemoteIdentifier() const;
    virtual void setRemoteIdentifier (const std::string &value, bool temporaritly = false);
    virtual InitState<bool> getPeerIsClient () const;
    virtual void setPeerIsClient (bool value, bool temporarily = false);

    /** the 1.0/1.1/1.2 part of the SyncMLVersion property */
    virtual InitStateString getSyncMLVersion() const;

    /** all flags that are set in the SyncMLVersion property, including the 1.0/1.1/1.2 versions */
    virtual InitState< std::set<std::string> > getSyncMLFlags() const;

    virtual InitState<unsigned int> getRequestMaxTime() const;

    /**
     * An arbitrary name assigned to the peer configuration,
     * not necessarily unique. Can be used by a GUI instead
     * of the config name. Unsetting it is supported.
     */
    virtual InitStateString getUserPeerName() const;
    virtual void setUserPeerName(const InitStateString &name);

    /**
     * The Device ID of our peer. Typically only relevant when the
     * peer is a client. Servers don't have a Device ID, just some
     * unique way of contacting them.
     */
    virtual InitStateString getRemoteDevID() const;
    virtual void setRemoteDevID(const std::string &value);

    /**
     * The opaque nonce value stored for a peer, required for MD5
     * authentication. Only used when acting as server.
     */
    virtual InitStateString getNonce() const;
    virtual void setNonce(const std::string &value);

    /**
     * The opaque per-peer admin data managed by the Synthesis
     * engine. Only used when acting as server.
     */
    virtual InitStateString getDeviceData() const;
    virtual void setDeviceData(const std::string &value);

    /**
     * Automatic sync related properties, used to control its behaviors
     */
    virtual InitStateString getAutoSync() const;
    virtual void setAutoSync(const std::string &value, bool temporarily = false);
    virtual InitState<unsigned int> getAutoSyncInterval() const;
    virtual void setAutoSyncInterval(unsigned int value, bool temporarily = false);
    virtual InitState<unsigned int> getAutoSyncDelay() const;
    virtual void setAutoSyncDelay(unsigned int value, bool temporarily = false);

    /**
     * Specifies whether WBXML is to be used (default).
     * Otherwise XML is used.
     */
    virtual InitState<bool> getWBXML() const;
    virtual void setWBXML(bool isWBXML, bool temporarily = false);

    virtual InitState<bool> getRefreshSync() const;
    virtual void setRefreshSync(bool enableRefreshSync, bool temporarily = false);

    virtual InitStateString getUserAgent() const { return "SyncEvolution"; }
    virtual InitStateString getMan() const { return "Patrick Ohly"; }
    virtual InitStateString getMod() const { return "SyncEvolution"; }
    virtual InitStateString getOem() const { return "Open Source"; }
    virtual InitStateString getHwv() const { return "unknown"; }
    virtual InitStateString getSwv() const;
    virtual InitStateString getDevType() const;
    /**@}*/

    enum Layout {
        SYNC4J_LAYOUT,        /**< .syncj4/evolution/<server>, SyncEvolution <= 0.7.x */
        HTTP_SERVER_LAYOUT,   /**< .config/syncevolution/<server> with sources
                                 underneath, SyncEvolution <= 0.9.x */
        SHARED_LAYOUT         /**< .config/syncevolution/<context> containing sources 
                                 and peers, with source settings shared by peers,
                                 SyncEvolution >= 1.0 */
    };

    /** config versioning; setting is done internally */
    int getConfigVersion(ConfigLevel level, ConfigLimit limit) const;

    /** file layout used by config */
    Layout getLayout() const { return m_layout; }

private:
    /**
     * scans for peer configurations
     * @param root         absolute directory path
     * @param configname   expected name of config files (config.ini or config.txt)
     * @retval res         filled with new peer configurations found
     */
    static void addPeers(const std::string &root,
                         const std::string &configname,
                         SyncConfig::ConfigList &res);

    /* internal access to configuration versioning */
    void setConfigVersion(ConfigLevel level, ConfigLimit limit, int version);

    /**
     * migrate root (""), context or peer config and everything contained in them to
     * the current config format
     */
    void migrate(const std::string &config);

    /**
     * set tree and nodes to VolatileConfigTree/Node
     */
    void makeVolatile();

    /**
     * String that identifies the peer, see constructor.
     * This is a normalized string (normalizePeerString()).
     * The name is a bit of a misnomer, because the config
     * might also reference just a context without any
     * peer-specific properties ("@some-context", or "@default").
     */
    std::string m_peer;

    /**
     * Lower case path to peer configuration,
     * relative to configuration tree root.
     * For example "scheduleworld" for "ScheduleWorld" when
     * using the old config layouts, "default/peers/scheduleworld"
     * in the new layout.
     *
     * Empty if configuration view has no peer-specific properties.
     */
    std::string m_peerPath;

    /**
     * lower case path to source set properties,
     * unused for old layouts, else something like
     * "default" or "other_context"
     */
    std::string m_contextPath;

    Layout m_layout;
    std::string m_redirectPeerRootPath;
    ConfigWriteMode m_configWriteMode;
    Bool m_ephemeral;

    /** holds all config nodes relative to the root that we found */
    boost::shared_ptr<ConfigTree> m_tree;

    /**
     * Same instance as m_tree, except that we know that it is a FileConfigTree.
     */
    boost::shared_ptr<FileConfigTree> m_fileTree;

    /** access to global sync properties, independent of
        the context (for example, "defaultPeer") */
    boost::shared_ptr<FilterConfigNode> m_globalNode;
    boost::shared_ptr<ConfigNode> m_globalHiddenNode;

    /** access to properties shared between peers */
    boost::shared_ptr<FilterConfigNode> m_contextNode;
    boost::shared_ptr<ConfigNode> m_contextHiddenNode;

    /** access to properties specific to a peer */
    boost::shared_ptr<FilterConfigNode> m_peerNode;
    boost::shared_ptr<ConfigNode> m_hiddenPeerNode;

    /** multiplexer for the other config nodes */
    boost::shared_ptr<FilterConfigNode> m_props[2];

    /**
     * temporary override for all sync source settings
     * ("" as key) or specific sources (source name as key)
     */
    SourceProps m_sourceFilters;

    static std::string getOldRoot() {
        return getHome() + "/.sync4j/evolution";
    }

    static std::string getNewRoot() {
        const char *xdg_root_str = getenv("XDG_CONFIG_HOME");
        return xdg_root_str ? std::string(xdg_root_str) + "/syncevolution" :
            getHome() + "/.config/syncevolution";
    }

    /** remember all SyncSourceNodes so that temporary changes survive */
    std::map<std::string, SyncSourceNodes> m_nodeCache;
};

/**
 * This set of config nodes is to be used by SyncSourceConfig
 * to accesss properties. Note that "const SyncSourceNodes"
 * only implies that the nodes cannot be changed; the properties
 * are still read- and writable. ConstSyncSourceNodes grants
 * only read access to properties.
 */
class SyncSourceNodes {
 public:
    SyncSourceNodes() : m_havePeerNode(false) {}

    /**
     * @param havePeerNode    false when peerNode is a dummy instance which has to
     *                        be ignored for properties which may exist there as
     *                        well as in the shared node (for example, "type")
     * @param sharedNode      node for user-visible properties, shared between peers
     * @param peerNode        node for user-visible, per-peer properties (the same
     *                        as sharedNode in SYNC4J_LAYOUT and HTTP_SERVER_LAYOUT)
     * @param hiddenPeerNode  node for internal, per-peer properties (the same as
     *                        sharedNode in SYNC4J_LAYOUT)
     * @param trackingNode    node for tracking changes (always different than the
     *                        other nodes)
     * @param serverNode      node for tracking items in a server (always different
     *                        than the other nodes)
     * @param cacheDir        a per-peer, per-source directory for exclusive use by
     *                        the SyncSource, must be created if needed; not available
     *                        when source is accessed independently of peer
     */
    SyncSourceNodes(bool havePeerNode,
                    const boost::shared_ptr<FilterConfigNode> &sharedNode,
                    const boost::shared_ptr<FilterConfigNode> &peerNode,
                    const boost::shared_ptr<ConfigNode> &hiddenPeerNode,
                    const boost::shared_ptr<ConfigNode> &trackingNode,
                    const boost::shared_ptr<ConfigNode> &serverNode,
                    const std::string &cacheDir);

    friend class SyncConfig;

    /** true if the peer-specific config node exists */
    bool exists() const { return m_peerNode->exists(); }

    /** true if the context-specific config node exists */
    bool dataConfigExists() const { return m_sharedNode->exists(); }

    /**
     * Returns the right config node for a certain property,
     * depending on visibility and sharing.
     */
    boost::shared_ptr<FilterConfigNode> getNode(const ConfigProperty &prop) const;

    /**
     * Read-write access to all configurable properties of the source.
     * The visible properties are passed through the config filter,
     * which can be modified.
     */
    boost::shared_ptr<FilterConfigNode> getProperties(bool hidden = false) const { return m_props[hidden]; }

    /** read-write access to SyncML server specific config node */
    boost::shared_ptr<ConfigNode> getServerNode() const { return m_serverNode; }

    /** read-write access to backend specific tracking node */
    boost::shared_ptr<ConfigNode> getTrackingNode() const { return m_trackingNode; }

    std::string getCacheDir() const { return m_cacheDir; }

 protected:
    const bool m_havePeerNode;
    const boost::shared_ptr<FilterConfigNode> m_sharedNode;
    const boost::shared_ptr<FilterConfigNode> m_peerNode;
    const boost::shared_ptr<ConfigNode> m_hiddenPeerNode;
    const boost::shared_ptr<ConfigNode> m_trackingNode;
    const boost::shared_ptr<ConfigNode> m_serverNode;
    const std::string m_cacheDir;

    /** multiplexer for the other nodes */
    boost::shared_ptr<FilterConfigNode> m_props[2];
};

/**
 * nop deleter for boost::shared_ptr<SyncConfig>
 */
struct SyncConfigNOP
{
    void operator() (SyncConfig *) {}
};

/**
 * same as SyncSourceNodes, but with only read access to properties
 */
class ConstSyncSourceNodes : private SyncSourceNodes
{
 public:
    ConstSyncSourceNodes(const SyncSourceNodes &other) :
       SyncSourceNodes(other)
    {}

    boost::shared_ptr<const FilterConfigNode> getProperties(bool hidden = false) const {
        return const_cast<SyncSourceNodes *>(static_cast<const SyncSourceNodes *>(this))->getProperties(hidden);
    }
    boost::shared_ptr<const ConfigNode> getServerNode() const { return m_serverNode; }
    boost::shared_ptr<const ConfigNode> getTrackingNode() const { return m_trackingNode; } 
};

struct SourceType {
    SourceType():m_forceFormat(false)
    {}

    /**
     * Parses the SyncEvolution <= 1.1 type specifier:
     * <backend>[:<format>[:<version>][!]]
     *
     * The <version> part is not stored anymore (was required by file
     * backend, but not actually used).
     */
    SourceType(const std::string &type);

    /**
     * converts back to SyncEvolution <= 1.1 type specifier
     */
    std::string toString() const;

    std::string m_backend; /**< identifies the SyncEvolution backend (either via a generic term like "addressbook" or a specific one like "Evolution Contacts") */
    std::string m_localFormat;  /**< the format to be used inside the backend for storing items; typically
                              hard-coded and not configurable */
    std::string m_format; /**< the format to be used (typically a MIME type) when talking to our peer */
    bool   m_forceFormat; /**< force to use the client's preferred format instead giving the engine and server a choice */
};

/**
 * This class maps per-source properties to ConfigNode properties.
 * Some properties are not configurable and have to be provided
 * by derived classes.
 */
class SyncSourceConfig {
 public:
    SyncSourceConfig(const std::string &name, const SyncSourceNodes &nodes);

    static ConfigPropertyRegistry &getRegistry();

    /**
     * Read-write access to all configurable properties of the source.
     * The visible properties are passed through the config filter,
     * which can be modified.
     */
    boost::shared_ptr<FilterConfigNode> getProperties(bool hidden = false) {
        return m_nodes.getProperties(hidden);
    }
    boost::shared_ptr<const FilterConfigNode> getProperties(bool hidden = false) const { return const_cast<SyncSourceConfig *>(this)->getProperties(hidden); }

    virtual std::string getName() const { return m_name; }

    /**
     * Directory to be used by source when it needs to store
     * something per-peer in the file system. Currently not
     * configurable, set via SyncSourceNodes.
     */
    std::string getCacheDir() const { return m_nodes.getCacheDir(); }

    /**
     * Returns the right config node for a certain property,
     * depending on visibility and sharing.
     */
    boost::shared_ptr<FilterConfigNode> getNode(const ConfigProperty &prop) {
        return m_nodes.getNode(prop);
    }
    boost::shared_ptr<const FilterConfigNode> getNode(const ConfigProperty &prop) const {
        return m_nodes.getNode(prop);
    }

    /** access to SyncML server specific config node */
    boost::shared_ptr<ConfigNode> getServerNode() { return m_nodes.getServerNode(); }
    boost::shared_ptr<const ConfigNode> getServerNode() const { return m_nodes.getServerNode(); }

    /** access to backend specific tracking node */
    boost::shared_ptr<ConfigNode> getTrackingNode() { return m_nodes.getTrackingNode(); }
    boost::shared_ptr<const ConfigNode> getTrackingNode() const { return m_nodes.getTrackingNode(); }

    /** sync mode for sync sources */
    static StringConfigProperty m_sourcePropSync;

    /** true if the source config exists with view-specific properties (not just default or shared ones) */
    bool exists() const { return m_nodes.exists(); }

    virtual UserIdentity getUser() const;
    virtual void setUsername(const std::string &value, bool temporarily = false);

    virtual InitStateString getPassword() const;
    virtual void setPassword(const std::string &value, bool temporarily = false);

    /** selects the backend database to use */
    virtual InitStateString getDatabaseID() const;
    virtual void setDatabaseID(const std::string &value, bool temporarily = false);

    /**
     * internal property: unique integer ID for the source, needed by Synthesis XML <dbtypeid>,
     * zero if unset
     */
    virtual InitState<int> getSynthesisID() const;
    virtual void setSynthesisID(int value, bool temporarily = false);

    /**
     * Returns the data source type configured as part of the given
     * configuration; different SyncSources then check whether
     * they support that type. This call has to work before instantiating
     * a source and thus gets passed a node to read from.
     */
    static InitState<SourceType> getSourceType(const SyncSourceNodes &nodes);
    virtual InitState<SourceType> getSourceType() const;

    /** set source backend and formats in one step */
    virtual void setSourceType(const SourceType &type, bool temporarily = false);

    virtual void setBackend(const std::string &value, bool temporarily = false);
    virtual InitStateString getBackend() const;
    virtual void setDatabaseFormat(const std::string &value, bool temporarily = false);
    virtual InitStateString getDatabaseFormat() const;
    virtual void setSyncFormat(const InitStateString &value, bool temporarily = false);
    virtual InitStateString getSyncFormat() const;
    virtual void setForceSyncFormat(bool value, bool temporarily = false);
    virtual InitState<bool> getForceSyncFormat() const;

    /**
     * Returns the SyncSource URI: used in SyncML to address the data
     * on the server.
     *
     * Each URI has to be unique during a sync session, i.e.
     * two different sync sources cannot access the same data at
     * the same time.
     */
    virtual InitStateString getURI() const;
    virtual void setURI(const std::string &value, bool temporarily = false);

    /**
     * like getURI(), but instead of returning an empty string when
     * not configured, return the source name
     */
    virtual InitStateString getURINonEmpty() const;


    /**
     * Gets the default syncMode.
     *
     * Sync modes can be one of:
     * - disabled
     * - slow
     * - two-way
     * - one-way-from-server
     * - one-way-from-client
     * - refresh-from-server
     * - refresh-from-client
     */
    virtual InitStateString getSync() const;
    virtual void setSync(const std::string &value, bool temporarily = false);

    /** shortcut for checking sync mode against "disabled" */
    bool isDisabled() { return getSync() == "disabled"; }

 private:
    // This can be called quite often, cache it.
    InitState<int> m_synthesisID;

    std::string m_name;
    SyncSourceNodes m_nodes;
};

class SingleFileConfigTree;

/**
 * Representing a configuration template node used for fuzzy matching.
 */
class TemplateConfig
{
    boost::shared_ptr<SingleFileConfigTree> m_template;
    ConfigProps m_metaProps;
    std::string m_id;
    std::string m_templateName;
public:
    TemplateConfig (const std::string &path);
    enum {
        NO_MATCH = 0,
        LEVEL1_MATCH = 1,
        LEVEL2_MATCH = 2,
        LEVEL3_MATCH = 3,
        LEVEL4_MATCH = 4,
        BEST_MATCH=5
    };
    static bool isTemplateConfig (const std::string &path);
    bool isTemplateConfig() const;
    virtual int metaMatch (const std::string &fingerprint, SyncConfig::MatchMode mode);
    virtual int serverModeMatch (SyncConfig::MatchMode mode);
    virtual int fingerprintMatch (const std::string &fingerprint);
    virtual std::string getTemplateId ();
    virtual std::string getDescription();
    virtual std::string getFingerprint();
    virtual std::string getTemplateName();
};


/**@}*/


SE_END_CXX
#endif
