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

#ifndef INCL_EVOLUTION_VOLATILE_CONFIG_NODE
# define INCL_EVOLUTION_VOLATILE_CONFIG_NODE

#include "FilterConfigNode.h"
#include "FileConfigNode.h"

/**
 * This class can store properties while in memory, but will never
 * save them persistently. Implemented by instantiating a FileConfigNode
 * with invalid path and never calling its flush() method.
 */
class VolatileConfigNode : public FilterConfigNode {
 public:
 VolatileConfigNode() :
    FilterConfigNode(boost::shared_ptr<ConfigNode>(new FileConfigNode("/dev/null", "dummy.ini")))
        {}

    virtual string getName() const { return "intermediate configuration"; }
    virtual void flush() {}
};

#endif
