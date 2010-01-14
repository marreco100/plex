/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */
#ifdef _WIN32
  #define _USE_MATH_DEFINES
#endif

#include "ConvolutionKernels.h"
#include "system.h"
#include "MathUtils.h"

#define SINC(x) (sin(M_PI * (x)) / (M_PI * (x)))

CConvolutionKernel::CConvolutionKernel(ESCALINGMETHOD method, int size)
{
  m_pixels = new float[size * 4];

  if (method == VS_SCALINGMETHOD_LANCZOS2)
    Lanczos2(size);
  else if (method == VS_SCALINGMETHOD_LANCZOS3_FAST)
    Lanczos3Fast(size);
  else if (method == VS_SCALINGMETHOD_LANCZOS3)
    Lanczos3(size);
  else if (method == VS_SCALINGMETHOD_CUBIC) 
    Bicubic(size, 1.0 / 3.0, 1.0 / 3.0);
}

CConvolutionKernel::~CConvolutionKernel()
{
  delete [] m_pixels;
}

//generate a lanczos2 kernel which can be loaded with RGBA format
//each value of RGBA has one tap, so a shader can load 4 taps with a single pixel lookup
void CConvolutionKernel::Lanczos2(int size)
{
  for (int i = 0; i < size; i++)
  {
    double x = (double)i / (double)size;

    //generate taps
    for (int j = 0; j < 4; j++)
      m_pixels[i * 4 + j] = (float)LanczosWeight(x + (double)(j - 2), 2.0);

    //any collection of 4 taps added together needs to be exactly 1.0
    //for lanczos this is not always the case, so we take each collection of 4 taps
    //and divide those taps by the sum of the taps
    float weight = 0.0;
    for (int j = 0; j < 4; j++)
      weight += m_pixels[i * 4 + j];

    for (int j = 0; j < 4; j++)
      m_pixels[i * 4 + j] /= weight;
  }
}

//generate a lanczos3 kernel which can be loaded with RGBA format
//each value of RGBA has one tap, so a shader can load 4 taps with a single pixel lookup
//the two outer lobes of the lanczos3 kernel are added to the two lobes one step to the middle
//this basically looks the same as lanczos3, but the kernel only has 4 taps,
//so it can use the 4x4 convolution shader which is twice as fast as the 6x6 one
void CConvolutionKernel::Lanczos3Fast(int size)
{
  for (int i = 0; i < size; i++)
  {
    double a = 3.0;
    double x = (double)i / (double)size;

    //generate taps
    m_pixels[i * 4 + 0] = (float)(LanczosWeight(x - 2.0, a) + LanczosWeight(x - 3.0, a));
    m_pixels[i * 4 + 1] = (float) LanczosWeight(x - 1.0, a);
    m_pixels[i * 4 + 2] = (float) LanczosWeight(x      , a);
    m_pixels[i * 4 + 3] = (float)(LanczosWeight(x + 1.0, a) + LanczosWeight(x + 2.0, a));

    //any collection of 4 taps added together needs to be exactly 1.0
    //for lanczos this is not always the case, so we take each collection of 4 taps
    //and divide those taps by the sum of the taps
    float weight = 0.0;
    for (int j = 0; j < 4; j++)
      weight += m_pixels[i * 4 + j];

    for (int j = 0; j < 4; j++)
      m_pixels[i * 4 + j] /= weight;
  }
}

//generate a lanczos3 kernel which can be loaded with RGBA format
//each value of RGB has one tap, so a shader can load 3 taps with a single pixel lookup
void CConvolutionKernel::Lanczos3(int size)
{
  for (int i = 0; i < size; i++)
  {
    double x = (double)i / (double)size;

    //generate taps
    for (int j = 0; j < 3; j++)
      m_pixels[i * 4 + j] = (float)LanczosWeight(x * 2.0 + (double)(j * 2 - 3), 3.0);

    m_pixels[i * 4 + 3] = 0.0;
  }

  //any collection of 6 taps added together needs to be exactly 1.0
  //for lanczos this is not always the case, so we take each collection of 6 taps
  //and divide those taps by the sum of the taps
  for (int i = 0; i < size / 2; i++)
  {
    float weight = 0.0;
    for (int j = 0; j < 3; j++)
    {
      weight += m_pixels[i * 4 + j];
      weight += m_pixels[(i + size / 2) * 4 + j];
    }
    for (int j = 0; j < 3; j++)
    {
      m_pixels[i * 4 + j] /= weight;
      m_pixels[(i + size / 2) * 4 + j] /= weight;
    }
  }
}

//generate a bicubic kernel which can be loaded with RGBA format
//each value of RGBA has one tap, so a shader can load 4 taps with a single pixel lookup
void CConvolutionKernel::Bicubic(int size, double B, double C)
{
  for (int i = 0; i < size; i++)
  {
    double x = (double)i / (double)size;

    //generate taps
    for (int j = 0; j < 4; j++)
      m_pixels[i * 4 + j] = (float)BicubicWeight(x + (double)(j - 2), B, C);
  }
}

double CConvolutionKernel::LanczosWeight(double x, double radius)
{
  double ax = fabs(x);

  if (ax == 0.0)
    return 1.0;
  else if (ax < radius)
    return SINC(ax) * SINC(ax / radius);
  else
    return 0.0;
}

double CConvolutionKernel::BicubicWeight(double x, double B, double C)
{
  double ax = fabs(x);

  if (ax<1.0)
  {
    return ((12 - 9*B - 6*C) * ax * ax * ax +
            (-18 + 12*B + 6*C) * ax * ax +
            (6 - 2*B))/6;
  }
  else if (ax<2.0)
  {
    return ((-B - 6*C) * ax * ax * ax + 
            (6*B + 30*C) * ax * ax + (-12*B - 48*C) * 
             ax + (8*B + 24*C)) / 6;
  }
  else
  {
    return 0.0;
  }
}
