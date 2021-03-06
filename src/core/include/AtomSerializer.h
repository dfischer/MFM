/* -*- mode:C++ -*- */

/*
  AtomSerializer.h A wrapper for (de)serializing an Atom
  Copyright (C) 2014 The Regents of the University of New Mexico.  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301
  USA
*/

/**
   \file AtomSerializer.h A wrapper for (de)serializing an Atom
   \author David H. Ackley.
   \date (C) 2014 All rights reserved.
   \lgpl
*/
#ifndef ATOMSERIALIZER_H
#define ATOMSERIALIZER_H

#include "itype.h"
#include "Atom.h"
#include "BitVector.h"
#include "ByteSerializable.h"
#include "LineCountingByteSource.h"

namespace MFM
{

  /**
   * An AtomTypeFormatter (somehow) maps from an atom to a text
   * representation of that atom's type, and (later) from that text
   * representation back to a default atom of the original atom's
   * element if it exists.  The primary reason this exists is that the
   * current simulator may be using a different element<->type number
   * mapping compared to the simulator that saved the atom's type.
   */
  template <class AC>
  class AtomTypeFormatter
  {
  public:
    typedef AC ATOM_CONFIG;

    // Extract short names for parameter types
    typedef typename ATOM_CONFIG::ATOM_TYPE T;

    AtomTypeFormatter() { }
    virtual ~AtomTypeFormatter() { }

    /**
       Read an atom type name (in some unspecified format) off the
       given LineCountingByteSource.  Return false (perhaps with an
       error message issued) if no legal element name can be read, in
       which case destAtom is unaltered.  Otherwise, set destAtom to a
       default atom of the type identified by the read name, and
       return true.  This operation must invert PrintAtomType() for
       any given AtomTypeFormatter.

       \sa PrintAtomType()
     */
    virtual bool ParseAtomType(LineCountingByteSource & bs, T & destAtom) = 0;

    /**
       Print the name (in some unspecified format) of the given atom
       on the given ByteSink.  Note that atom might be insane or have
       an illegal type, and if so, the AtomTypeFormatter must print
       some representation that reliably causes its ParseAtomType() to
       return false given that same representation.

       This operation must be inverted by ParseAtomType() for any
       given AtomTypeFormatter.

       \sa ParseAtomType()
     */
    virtual void PrintAtomType(const T & atom, ByteSink & bs) = 0;
  };


  /**
   * A wrapper class to use for making an Atom ByteSerializable.  We
   * don' make Atom itself ByteSerializable because ByteSerializable
   * has virtual functions and we're not going to pay for a vtable
   * pointer in each Atom..
   */
  template <class AC>
  class AtomSerializer : public ByteSerializable
  {
   private:
    enum { BPA = AC::BITS_PER_ATOM };

    Atom<AC> & m_atom;

   public:
    AtomSerializer(Atom<AC> & atom) : m_atom(atom)
    { }

    Result PrintTo(ByteSink & bs, s32 argument = 0)
    {
      m_atom.m_bits.Print(bs);
      return SUCCESS;
    }

    Result ReadFrom(ByteSource & bs, s32 argument = 0)
    {
      if (m_atom.m_bits.Read(bs))
        return SUCCESS;
      return FAILURE;
    }

    const BitVector<BPA> & GetBits() const
    {
      return m_atom.m_bits;
    }

  };
} /* namespace MFM */

#endif /*ATOMSERIALIZER_H*/
