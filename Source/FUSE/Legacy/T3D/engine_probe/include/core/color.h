#ifndef _COLOR_H_
#define _COLOR_H_

#ifndef _TORQUE_TYPES_H_
#include "platform/types.h"
#endif

const F32 gOneOver255 = 1.f / 255.f;

class ColorI
{
public:
   U8 red;
   U8 green;
   U8 blue;
   U8 alpha;

   ColorI() : red(0), green(0), blue(0), alpha(0) {}
   ColorI(U8 r, U8 g, U8 b, U8 a = 255) : red(r), green(g), blue(b), alpha(a) {}

   void set(U8 in_r, U8 in_g, U8 in_b, U8 in_a = 255)
   {
      red = in_r;
      green = in_g;
      blue = in_b;
      alpha = in_a;
   }
};

class LinearColorF
{
public:
   F32 red;
   F32 green;
   F32 blue;
   F32 alpha;

   LinearColorF() : red(0), green(0), blue(0), alpha(0) {}
   LinearColorF(F32 r, F32 g, F32 b, F32 a = 1.0f) : red(r), green(g), blue(b), alpha(a) {}
   LinearColorF(const ColorI& color)
      : red(static_cast<F32>(color.red) * gOneOver255),
        green(static_cast<F32>(color.green) * gOneOver255),
        blue(static_cast<F32>(color.blue) * gOneOver255),
        alpha(static_cast<F32>(color.alpha) * gOneOver255)
   {
   }

   ColorI toColorI(const bool /*keepAsLinear*/ = false) const
   {
      ColorI out;
      out.red = static_cast<U8>(red * 255.f);
      out.green = static_cast<U8>(green * 255.f);
      out.blue = static_cast<U8>(blue * 255.f);
      out.alpha = static_cast<U8>(alpha * 255.f);
      return out;
   }
};

#endif // _COLOR_H_
