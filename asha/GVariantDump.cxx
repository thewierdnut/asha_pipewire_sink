#include "GVariantDump.hh"

#include <glib-2.0/glib.h>

#include <cassert>
#include <iomanip>
#include <ostream>
#include <memory>


void GVariantDump(GVariant* v, std::ostream& out, const std::string& whitespace)
{
   constexpr char tab[] = "  ";
   const char* type = g_variant_get_type_string(v);
   assert(type);

   switch(type[0])
   {
   //case G_VARIANT_CLASS_MAYBE: // TODO
   case G_VARIANT_CLASS_ARRAY:
      if (type[1] == '{') // Dictionary
      {
         gsize n = g_variant_n_children(v);
         if (n == 0)
         {
            out << "{}";
         }
         else
         {
            out << "{";
            for (size_t i = 0; i < n; ++i)
            {
               std::shared_ptr<GVariant> kv(g_variant_get_child_value(v, i), g_variant_unref);
               std::shared_ptr<GVariant> k(g_variant_get_child_value(kv.get(), 0), g_variant_unref);
               std::shared_ptr<GVariant> v(g_variant_get_child_value(kv.get(), 1), g_variant_unref);
               if (i != 0) out << ',';
               out << '\n' << whitespace + tab;
               GVariantDump(k.get(), out, whitespace + tab);
               out << ": ";
               GVariantDump(v.get(), out, whitespace + tab);
            }

            out << '\n' << whitespace << "}";
         }
      }
      else if (type[1] == 'y') // Byte array
      {
         gsize n = g_variant_n_children(v);
         if (n == 0)
         {
            out << "[]";
         }
         else
         {
            out << "[";
            for (size_t i = 0; i < n; ++i)
            {
               std::shared_ptr<GVariant> e(g_variant_get_child_value(v, i), g_variant_unref);
               if (i != 0) out << ", ";
               GVariantDump(e.get(), out, whitespace + tab);
            }
            out << ']';
         }
      }
      else // Other
      {
         gsize n = g_variant_n_children(v);
         if (n == 0)
         {
            out << "[]";
         }
         else
         {
            out << "[";
            for (size_t i = 0; i < n; ++i)
            {
               std::shared_ptr<GVariant> e(g_variant_get_child_value(v, i), g_variant_unref);
               if (i != 0) out << ", ";
               out << '\n' << whitespace + tab;
               GVariantDump(e.get(), out, whitespace + tab);
            }
            out << '\n' << whitespace << ']';
         }
      }
      break;
   case G_VARIANT_CLASS_TUPLE:
   {
      size_t n = g_variant_n_children(v);
      out << '(';
      for (size_t i = 0; i < n; ++i)
      {
         if (i != 0) out << ", ";
         std::shared_ptr<GVariant> e(g_variant_get_child_value(v, i), g_variant_unref);
         GVariantDump(e.get(), out, whitespace);
      }

      out << ')';
      break;
   }
   case G_VARIANT_CLASS_DICT_ENTRY:
   {
      std::shared_ptr<GVariant> key(g_variant_get_child_value(v, 0), g_variant_unref);
      std::shared_ptr<GVariant> val(g_variant_get_child_value(v, 1), g_variant_unref);
      
      out << '{';
      GVariantDump(key.get(), out, whitespace);
      out << ", ";
      GVariantDump(val.get(), out, whitespace);
      out << '}';
      break;
   }
   case G_VARIANT_CLASS_VARIANT:
   {
      std::shared_ptr<GVariant> var(g_variant_get_variant(v), g_variant_unref);
      out << '<';
      // out << g_variant_get_type_string(var.get()) << ' ';
      GVariantDump(var.get(), out, whitespace);
      out << '>';
      break;
   }
   case G_VARIANT_CLASS_BOOLEAN:
      out << (g_variant_get_boolean(v) ? "true" : "false");
      break;
   case G_VARIANT_CLASS_OBJECT_PATH:
   case G_VARIANT_CLASS_SIGNATURE:
   case G_VARIANT_CLASS_STRING:
      out << '"' << g_variant_get_string(v, nullptr) << '"';
      break;
   case G_VARIANT_CLASS_BYTE:
   {
      std::ios_base::fmtflags old_flags(out.flags());
      out << "0x" << std::setw(2)  << std::setfill('0') << std::hex << (unsigned)g_variant_get_byte(v);
      out.flags(old_flags);
      break;
   }
   case G_VARIANT_CLASS_UINT16:
      out << g_variant_get_uint16(v);
      break;
   case G_VARIANT_CLASS_INT16:
      out << g_variant_get_int16(v);
      break;
   case G_VARIANT_CLASS_UINT32:
      out << g_variant_get_uint32(v);
      break;
   case G_VARIANT_CLASS_INT32:
      out << g_variant_get_int32(v);
      break;
   case G_VARIANT_CLASS_HANDLE:
      out << g_variant_get_handle(v);
      break;
   case G_VARIANT_CLASS_UINT64:
      out << g_variant_get_uint64(v);
      break;
   case G_VARIANT_CLASS_INT64:
      out << g_variant_get_int64(v);
      break;
   case G_VARIANT_CLASS_DOUBLE:
      out << g_variant_get_double(v);
      break;
   default:
      out << "???";
   }
}

std::string GVariantDump(GVariant* v)
{
   std::stringstream ss;
   GVariantDump(v, ss);
   return ss.str();
}