#pragma once

#include <glib-2.0/glib.h>

#include <ostream>
#include <memory>

void GVariantDump(GVariant* v, std::ostream& out, const std::string& whitespace = "");
std::string GVariantDump(GVariant* v);

inline std::string GVariantDump(const std::shared_ptr<_GVariant>& v) { return GVariantDump(v.get()); }
inline std::ostream& operator<< (std::ostream& o, GVariant* v)
{
   GVariantDump(v, o);
   return o;
}

int16_t GVariantToInt16(const std::shared_ptr<_GVariant>& v);
bool GVariantToBool(const std::shared_ptr<_GVariant>& v);