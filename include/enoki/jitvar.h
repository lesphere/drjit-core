/*
    enoki/array.h -- Simple C++ array classes with operator overloading

    This library implements convenient wrapper classes around the C API in
    'enoki/jit.h'.

    Copyright (c) 2020 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#pragma once

#include <enoki/jit.h>
#include <enoki/traits.h>
#include <cstring>
#include <cstdio>

template <typename Value>
struct CUDAArray {
    static constexpr VarType Type = var_type_v<Value>;

    CUDAArray() = default;

    ~CUDAArray() { jitc_var_dec_ref_ext(m_index); }

    CUDAArray(const CUDAArray &a) : m_index(a.m_index) {
        jitc_var_inc_ref_ext(m_index);
    }

    CUDAArray(CUDAArray &&a) : m_index(a.m_index) {
        a.m_index = 0;
    }

    template <typename T> CUDAArray(const CUDAArray<T> &v) {
        const char *op;

        if (std::is_floating_point_v<T> && std::is_integral_v<Value>)
            op = "cvt.rzi.$t1.$t2 $r1, $r2";
        else if (std::is_integral_v<T> && std::is_floating_point_v<Value>)
            op = "cvt.rn.$t1.$t2 $r1, $r2";
        else
            op = "cvt.$t1.$t2 $r1, $r2";

        m_index = jitc_trace_append_1(Type, op, v.index_());
    }

    CUDAArray(Value value) {
        const char *fmt = nullptr;

        switch (Type) {
            case VarType::Float16:
                fmt = "mov.$t1 $r1, %04x";
                break;

            case VarType::Float32:
                fmt = "mov.$t1 $r1, 0f%08x";
                break;

            case VarType::Float64:
                fmt = "mov.$t1 $r1, 0d%016llx";
                break;

            case VarType::Bool:
                fmt = "mov.$t1 $r1, %i";
                break;

            case VarType::Int8:
            case VarType::UInt8:
                fmt = "mov.$t1 $r1, 0x%02x";
                break;

            case VarType::Int16:
            case VarType::UInt16:
                fmt = "mov.$t1 $r1, 0x%04x";
                break;

            case VarType::Int32:
            case VarType::UInt32:
                fmt = "mov.$t1 $r1, 0x%08x";
                break;

            case VarType::Pointer:
            case VarType::Int64:
            case VarType::UInt64:
                fmt = "mov.$t1 $r1, 0x%016llx";
                break;

            default:
                fmt = "<<invalid format during cast>>";
                break;
        }

        uint_with_size_t<Value> value_uint;
        char value_str[32];
        memcpy(&value_uint, &value, sizeof(Value));
        snprintf(value_str, 32, fmt, value_uint);

        m_index = jitc_trace_append_0(Type, value_str);
    }

    template <typename... Args, enable_if_t<(sizeof...(Args) > 1)> = 0>
    CUDAArray(Args&&... args) {
        Value data[] = { (Value) args... };
        m_index = jitc_var_copy_to_device(Type, sizeof...(Args), data);
    }

    CUDAArray &operator=(const CUDAArray &a) {
        jitc_var_inc_ref_ext(a.m_index);
        jitc_var_dec_ref_ext(m_index);
        m_index = a.m_index;
        return *this;
    }

    CUDAArray &operator=(CUDAArray &&a) {
        std::swap(m_index, a.m_index);
        return *this;
    }

private:
    uint32_t m_index;
};
