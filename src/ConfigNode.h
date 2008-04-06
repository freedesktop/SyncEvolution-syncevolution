/*
 * Copyright (C) 2008 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY, TITLE, NONINFRINGEMENT or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307  USA
 */

#ifndef INCL_EVOLUTION_ABSTRACT_CONFIG_NODE
# define INCL_EVOLUTION_ABSTRACT_CONFIG_NODE

#include <map>
#include <utility>
#include <string>
using namespace std;

/**
 * This class corresponds to the Funambol C++ client
 * DeviceManagementNode, but offers a slightly different API.  See
 * ConfigTree for details.
 */
class ConfigNode {
 public:
    /** free resources without saving */
    virtual ~ConfigNode() {}

    /** a name for the node that the user can understand */
    virtual string getName() const = 0;

    /**
     * save all changes persistently
     */
    virtual void flush() = 0;

    /**
     * Returns the value of the given property
     *
     * @param property - the property name
     * @return value of the property or empty string if not set
     */
    virtual string readProperty(const string &property) const = 0;

    /**
     * Sets a property value.
     *
     * @param property   the property name
     * @param value      the property value (zero terminated string)
     * @param comment    a comment explaining what the property is about, with
     *                   \n separating lines; might be used by the backend 
     *                   when adding a new property
     * @param defValue   If a defValue is provided and the value
     *                   matches the default, then the node is asked to
     *                   remember that the value hasn't really been changed.
     *                   An implementation can decide to not support this.
     */
    virtual void setProperty(const string &property,
                             const string &value,
                             const string &comment = string(""),
                             const string *defValue = NULL) = 0;

    /**
     * Extract all list of all currently defined properties
     * and their values. Does not include values which were
     * initialized with their defaults, if the implementation
     * remembers that.
     */
    virtual map<string, string> readProperties() const = 0;

    /**
     * Remove a certain property.
     *
     * @param property    the name of the property which is to be removed
     */
    virtual void removeProperty(const string &property) = 0;

    /**
     * Node exists in backend storage.
     */
    virtual bool exists() const = 0;
};

#endif
