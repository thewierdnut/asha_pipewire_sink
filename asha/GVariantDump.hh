#pragma once

#include <glib-2.0/glib.h>

#include <ostream>

void GVariantDump(GVariant* v, std::ostream& out, const std::string& whitespace = "");

inline std::ostream& operator<< (std::ostream& o, GVariant* v)
{
   GVariantDump(v, o);
   return o;
}