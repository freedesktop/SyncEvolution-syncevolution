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

#ifndef INCL_EVOLUTION_FILE_CONFIG_NODE
# define INCL_EVOLUTION_FILE_CONFIG_NODE

#include "ConfigNode.h"

#include <string>
#include <list>
using namespace std;

/**
 * This class started its life as the Posix implementation of the
 * ManagementNode in the Funambol C++ client library. Nowadays it is
 * part of the SyncEvoluition ConfigTree (see there for details).
 *
 * Each node is mapped to one file whose location is determined by
 * the ConfigTree when the node gets created. Each node represents
 * one .ini file with entries of the type
 * <property>\s*=\s*<value>\s*\n
 *
 * Comments look like:
 * \s*# <comment>
 *
 * @todo rewrite with standard C++ containers
 */
class FileConfigNode : public ConfigNode {
    string m_path;
    string m_fileName;

    list<string> m_lines;
    bool m_modified;
    bool m_exists;

    void read();

 public:
    /**
     * Open or create a new file. The file will be physically created
     * right away whereas changes to its content will not be written
     * immediately.
     *
     * @param path      node name, maps to directory
     * @param fileName  name of file inside that directory
     */
    FileConfigNode(const string &path, const string &fileName);

    virtual string getName() const { return m_path + "/" + m_fileName; }

    virtual void flush();
    virtual string readProperty(const string &property) const;
    virtual void setProperty(const string &property,
                             const string &value,
                             const string &comment = "",
                             const string *defValue = NULL);
    virtual map<string, string> readProperties() const;
    virtual void removeProperty(const string &property);
    virtual bool exists() const { return m_exists; }
};

#endif
