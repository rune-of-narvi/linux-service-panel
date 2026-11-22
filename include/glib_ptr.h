#pragma once

#include <glib-object.h>

#include <memory>

template <typename T, auto Release> struct GlibDeleter
{
    void operator()(T* value) const
    {
        Release(value);
    }
};

template <typename T, auto Release> using GlibPtr = std::unique_ptr<T, GlibDeleter<T, Release>>;

template <typename T> using ObjectPtr = GlibPtr<T, g_object_unref>;

using GStringPtr = GlibPtr<char, g_free>;
using ErrorPtr = GlibPtr<GError, g_error_free>;
using VariantPtr = GlibPtr<GVariant, g_variant_unref>;
