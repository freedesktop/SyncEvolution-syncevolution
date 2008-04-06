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

#include "SafeConfigNode.h"
#include "EvolutionSyncClient.h"

#include <boost/foreach.hpp>

SafeConfigNode::SafeConfigNode(const boost::shared_ptr<ConfigNode> &node) :
    m_node(node),
    m_readOnlyNode(node)
{
}

SafeConfigNode::SafeConfigNode(const boost::shared_ptr<const ConfigNode> &node) :
    m_readOnlyNode(node)
{
}

string SafeConfigNode::readProperty(const string &property) const
{
    return unescape(m_readOnlyNode->readProperty(escape(property)));
}

void SafeConfigNode::setProperty(const string &property,
                                 const string &value,
                                 const string &comment,
                                 const string *defValue)
{
    m_node->setProperty(escape(property),
                        escape(value),
                        comment,
                        defValue);
}

map<string, string> SafeConfigNode::readProperties() const
{
    map<string, string> original = m_readOnlyNode->readProperties(),
        res;

    for(map<string, string>::iterator it = original.begin();
        it != original.end();
        ++it) {
        string key = unescape(it->first);
        string value = unescape(it->second);

        res.insert(make_pair(key, value));
    }

    return res;
}

void SafeConfigNode::removeProperty(const string &property)
{
    m_node->removeProperty(escape(property));
}

void SafeConfigNode::flush()
{
    if (!m_node.get()) {
        EvolutionSyncClient::throwError(getName() + ": read-only, flushing allowed");
    }
    m_node->flush();
}

string SafeConfigNode::escape(const string &str)
{
    string res;
    char buffer[4];
    res.reserve(str.size() * 3);

    BOOST_FOREACH(char c, str) {
        if(isalnum(c) ||
           c == '-' ||
           c == '_') {
            res += c;
        } else {
            sprintf(buffer, "!%02x",
                    (unsigned int)(unsigned char)c);
            res += buffer;
        }
    }

    return res;
}

string SafeConfigNode::unescape(const string &str)
{
    string res;
    size_t curr;

    res.reserve(str.size());

    curr = 0;
    while (curr < str.size()) {
        if (str[curr] == '!') {
            string hex = str.substr(curr + 1, 2);
            res += (char)strtol(hex.c_str(), NULL, 16);
            curr += 3;
        } else {
            res += str[curr];
            curr++;
        }
    }

    return res;
}
