/*
 * This file is a part of Luminance HDR package.
 * ----------------------------------------------------------------------
 * Copyright (C) 2003,2004 Rafal Mantiuk and Grzegorz Krawczyk
 * Copyright (C) 2010-2012 Davide Anastasia (Luminance HDR)
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
 */

//! \file array2d.cpp
//! \brief PFS library - general 2d array interface
//! \author Rafal Mantiuk, <mantiuk@mpi-sb.mpg.de>
//! $Id: array2d.h,v 1.1 2005/06/15 13:36:55 rafm Exp $
//!
//! \author Davide Anastasia <davideanastasia@users.sourceforge.net>
//! \note This version is different then the one in the PFSTOOLS

#include <iostream>
#include <assert.h>
#include <arch/malloc.h>

#include "array2d.h"
#include "vex.h"
#include "vex/vex.h"

using namespace std;

namespace pfs
{  
Array2D::Array2D(int cols, int rows)
    : m_cols(cols)
    , m_rows(rows)
    // aligned memory allocation allows faster vectorized access
    , m_data( static_cast<float*>(_mm_malloc(cols*rows*sizeof(float), 32)) )
    , m_isOwned( true )
{}

Array2D::Array2D(int cols, int rows, float* data)
    : m_cols(cols)
    , m_rows(rows)
    , m_data(data)
    , m_isOwned( false )
{}

// copy constructor?
//Array2D::Array2D(const Array2D& other)
//{
//    this->m_cols = other.m_cols;
//    this->m_rows = other.m_rows;
//    this->m_data = other.m_data;
//    this->m_isOwned = false;
//}
//
//// Assignment operator
//Array2D& Array2D::operator=(const Array2D& other)
//{
//    if (m_isOwned) _mm_free(m_data);
//
//    this->m_cols = other.m_cols;
//    this->m_rows = other.m_rows;
//    this->m_data = other.m_data;
//    this->m_isOwned = false;
//    return *this;
//}

Array2D::~Array2D()
{
    if (m_isOwned)
    {
        _mm_free(m_data);
    }
}

void Array2D::reset(const float value)
{
    VEX_vset(this->m_data, value, this->m_rows*this->m_cols);
}

void Array2D::scale(const float value)
{
    // O[i] = c * I[i]
    VEX_vsmul(this->m_data, value, this->m_data, this->m_rows*this->m_cols);
}

void copyArray(const Array2D *from, Array2D *to)
{
    assert( from->getRows() == to->getRows() );
    assert( from->getCols() == to->getCols() );

    const float* f = from->getRawData();
    float* t = to->getRawData();

    const int V_ELEMS = from->getRows()*from->getCols();

    VEX_vcopy(f, t, V_ELEMS);
}

void copyArray(const Array2D *from, Array2D *to, int x_ul, int y_ul, int x_br, int y_br)
{
    const float* fv = from->getRawData();
    float* tv       = to->getRawData();

    const int IN_W    = from->getCols();
    const int IN_H    = from->getRows();
    const int OUT_W   = to->getCols();
    const int OUT_H   = to->getRows();

    assert( OUT_H <= IN_H );
    assert( OUT_H <= IN_H );
    assert( x_ul >= 0 );
    assert( y_ul >= 0 );
    assert( x_br <= IN_W );
    assert( y_br <= IN_H );

    // move to row (x_ul, y_ul)
    fv = &fv[IN_W*y_ul + x_ul];

#pragma omp parallel for
    for (int r = 0; r < OUT_H; r++)
    {
        //NOTE: do NOT use VEX_vcopy
        for (int c = 0; c < OUT_W; c++)
        {
            tv[r*OUT_W + c] = fv[r*IN_W + c];
        }
    }
}

void setArray(Array2D *array, const float value)
{
    array->reset(value);
}

void multiplyArray(Array2D *z, const Array2D *x, const Array2D *y)
{
    assert( x->getRows() == y->getRows() );
    assert( x->getCols() == y->getCols() );
    assert( x->getRows() == z->getRows() );
    assert( x->getCols() == z->getCols() );

    vex::vmul(x->getRawData(), y->getRawData(),
              z->getRawData(),
              x->getRows()*x->getCols());
}

void divideArray(Array2D *z, const Array2D *x, const Array2D *y)
{
    assert( x->getRows() == y->getRows() );
    assert( x->getCols() == y->getCols() );
    assert( x->getRows() == z->getRows() );
    assert( x->getCols() == z->getCols() );

    const float* xv = x->getRawData();
    const float* yv = y->getRawData();
    float* zv = z->getRawData();

    const int elements = x->getRows()*x->getCols();

    VEX_vdiv(xv, yv, zv, elements);
}
}




