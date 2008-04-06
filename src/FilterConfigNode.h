/*
 * Copyright (C) 2003-2007 Funambol, Inc
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

#ifndef INCL_EVOLUTION_FILTER_CONFIG_NODE
# define INCL_EVOLUTION_FILTER_CONFIG_NODE

#include <ConfigNode.h>
#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string/case_conv.hpp>

#include "SyncEvolutionUtil.h"

#include <map>
#include <utility>
#include <vector>
#include <string>
using namespace std;

/**
 * This class acts as filter between a real config node and its user:
 * reads which match properties which are set in the filter will
 * return the value set in the filter. Writes will go to the underlying
 * node and future reads will return the written value.
 *
 * The purpose of this class is temporarily overriding saved values
 * during one run without having to modify the saved values.
 */
class FilterConfigNode : public ConfigNode {
 public:
    class ConfigFilter : public map<string, string> {
    public:
        /** add the mapping, regardless whether it exists already or not */
        void set(const string &property, const string &value) {
            pair<iterator, bool> inserted = insert(make_pair(boost::to_lower_copy(property), value));
            if (!inserted.second) {
                inserted.first->second = value;
            }
        }

        operator string () const {
            vector<string> res;

            for (const_iterator it = begin();
                 it != end();
                 ++it) {
                res.push_back(it->first + " = " + it->second);
            }
            sort(res.begin(), res.end());
            return join("\n", res.begin(), res.end());
        }

        iterator find(const string &property) {
            return map<string, string>::find(boost::to_lower_copy(property));
        }

        const_iterator find(const string &property) const {
            return map<string, string>::find(boost::to_lower_copy(property));
        }
    };

    /** read-write access to underlying node */
    FilterConfigNode(const boost::shared_ptr<ConfigNode> &node,
                     const ConfigFilter &filter = ConfigFilter());

    /** read-only access to underlying node */
    FilterConfigNode(const boost::shared_ptr<const ConfigNode> &node,
                     const ConfigFilter &filter = ConfigFilter());

    virtual string getName() const { return m_readOnlyNode->getName(); }

    /** add another entry to the list of filter properties */
    void addFilter(const string &property,
                   const string &value);

    /** replace current filter list with new one */
    void setFilter(const ConfigFilter &filter);
    const ConfigFilter &getFilter() const { return m_filter; }

    /* ConfigNode API */
    virtual void flush();
    virtual string readProperty(const string &property) const;
    virtual void setProperty(const string &property,
                             const string &value,
                             const string &comment = "",
                             const string *defValue = NULL);
    virtual map<string, string> readProperties() const;
    virtual void removeProperty(const string &property);
    virtual bool exists() const { return m_readOnlyNode->exists(); }

 private:
    ConfigFilter m_filter;
    boost::shared_ptr<ConfigNode> m_node;
    boost::shared_ptr<const ConfigNode> m_readOnlyNode;
};

#endif
