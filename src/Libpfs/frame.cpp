/**
 * @brief PFS library - Frame
 *
 * This file is a part of Luminance HDR package.
 * ----------------------------------------------------------------------
 * Copyright (C) 2003,2004 Rafal Mantiuk and Grzegorz Krawczyk
 * Copyright (C) 2011 Davide Anastasia
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * ----------------------------------------------------------------------
 *
 * @author Rafal Mantiuk, <mantiuk@mpi-sb.mpg.de>
 * @author Davide Anastasia <davideanastasia@users.sourceforge.net>
 *   Frame definition split from pfs.cpp
 */

#include <iostream>
#include <algorithm>

#include "frame.h"
#include "domio.h"
#include "channel.h"

using namespace std;

namespace pfs
{    
Frame::Frame( int width, int height )
    : m_width( width )
    , m_height( height )
{}

namespace
{
struct ChannelDeleter
{
    template <typename T>
    inline
    void operator()(T* p)
    {
        delete p;
    }
};
}


Frame::~Frame()
{
    for_each(m_channels.begin(),
             m_channels.end(),
             ChannelDeleter());
}

namespace
{
struct FindChannel
{
    FindChannel(const std::string& nameChannel)
        : nameChannel_(nameChannel)
    {}

    inline
    bool operator()(const Channel* channel) const
    {
        return !(channel->getName().compare( nameChannel_ ));
    }

private:
    std::string nameChannel_;
};
}

void Frame::getXYZChannels(const Channel* &X, const Channel* &Y, const Channel* &Z ) const
{
    // find X
    ChannelContainer::const_iterator it(
                std::find_if(m_channels.begin(),
                             m_channels.end(),
                             FindChannel("X"))
                );
    if ( it == m_channels.end() )
    {
        X = Y = Z = NULL;
        return;
    }
    X = *it;

    // find Y
    it = std::find_if(m_channels.begin(),
                      m_channels.end(),
                      FindChannel("Y"));
    if ( it == m_channels.end() )
    {
        X = Y = Z = NULL;
        return;
    }
    Y = *it;

    // find Y
    it = std::find_if(m_channels.begin(),
                      m_channels.end(),
                      FindChannel("Z"));
    if ( it == m_channels.end() )
    {
        X = Y = Z = NULL;
        return;
    }
    Z = *it;
}

void Frame::getXYZChannels( Channel* &X, Channel* &Y, Channel* &Z )
{
    const Channel* X_;
    const Channel* Y_;
    const Channel* Z_;

    static_cast<const Frame&>(*this).getXYZChannels(X_, Y_, Z_);

    X = const_cast<Channel*>(X_);
    Y = const_cast<Channel*>(Y_);
    Z = const_cast<Channel*>(Z_);
}

void Frame::createXYZChannels( Channel* &X, Channel* &Y, Channel* &Z )
{
    X = createChannel("X");
    Y = createChannel("Y");
    Z = createChannel("Z");
}

const Channel* Frame::getChannel(const std::string &name) const
{
    ChannelContainer::const_iterator it = std::find_if(m_channels.begin(),
                                              m_channels.end(),
                                              FindChannel(name));
    if ( it == m_channels.end() )
        return NULL;
    else
        return *it;
}

Channel* Frame::getChannel(const std::string& name)
{
    return const_cast<Channel*>(static_cast<const Frame&>(*this).getChannel(name));
}

Channel* Frame::createChannel(const std::string& name)
{
    ChannelContainer::iterator it = std::find_if(m_channels.begin(),
                                              m_channels.end(),
                                              FindChannel(name));
    if ( it != m_channels.end() )
    {
        return *it;
    }
    else
    {
        Channel* ch = new Channel( m_width, m_height, name );

        m_channels.push_back( ch );

        return ch;
    }
}

void Frame::removeChannel(const std::string& channel)
{
    ChannelContainer::iterator it = std::find_if(m_channels.begin(),
                                                 m_channels.end(),
                                                 FindChannel(channel));
    if ( it != m_channels.end() )
    {
        Channel* ch = *it;
        m_channels.erase( it );
        delete ch;
    }
}

ChannelContainer& Frame::getChannels()
{
    return this->m_channels;
}

const ChannelContainer& Frame::getChannels() const
{
    return this->m_channels;
}

TagContainer& Frame::getTags()
{
    return m_tags;
}

const TagContainer& Frame::getTags() const
{
    return m_tags;
}

} // namespace pfs


