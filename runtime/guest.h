// Calling between host and guest code: argument marshalling for kernel imports, and GuestCall.
#pragma once
#include "ppc_config.h"
#include <ppc_context.h>
#include <xbox.h>
#include <tuple>
#include <type_traits>
#include "log.h"
#include "memory.h"

namespace rt
{
    // Guest pointer argument or result: a 32-bit guest address viewed as T in host memory.
    template<typename T>
    struct GuestPtr
    {
        uint32_t address = 0;

        T* get() const { return Translate<T>(address); }
        T* operator->() const { return get(); }
        T& operator*() const { return *get(); }
        explicit operator bool() const { return address != 0; }
    };

    inline constexpr PPCRegister PPCContext::* Gprs[] = {
        &PPCContext::r3, &PPCContext::r4, &PPCContext::r5, &PPCContext::r6,
        &PPCContext::r7, &PPCContext::r8, &PPCContext::r9, &PPCContext::r10,
    };

    inline constexpr PPCRegister PPCContext::* Fprs[] = {
        &PPCContext::f1, &PPCContext::f2, &PPCContext::f3, &PPCContext::f4,
        &PPCContext::f5, &PPCContext::f6, &PPCContext::f7, &PPCContext::f8,
        &PPCContext::f9, &PPCContext::f10, &PPCContext::f11, &PPCContext::f12, &PPCContext::f13,
    };

    template<typename T> struct IsGuestPtr : std::false_type {};
    template<typename T> struct IsGuestPtr<GuestPtr<T>> : std::true_type {};

    // Integer argument slot i. Slots past r10 live in the caller's parameter area: 8-byte slots
    // starting at r1 + 0x50, with 32-bit values in the low word.
    inline uint64_t IntArg(PPCContext& ctx, size_t i, size_t bytes)
    {
        if (i < 8)
            return (ctx.*Gprs[i]).u64;
        const uint32_t slot = ctx.r1.u32 + 0x50 + uint32_t(i - 8) * 8;
        return bytes > 4 ? uint64_t(*Translate<be<uint64_t>>(slot)) : uint64_t(*Translate<be<uint32_t>>(slot + 4));
    }

    template<typename T>
    T ReadArg(PPCContext& ctx, size_t& gpr, size_t& fpr)
    {
        if constexpr (std::is_same_v<T, PPCContext&>)
            return ctx;
        else if constexpr (std::is_floating_point_v<T>)
            return T((ctx.*Fprs[fpr++]).f64);
        else if constexpr (IsGuestPtr<T>::value)
            return T{ uint32_t(IntArg(ctx, gpr++, 4)) };
        else if constexpr (std::is_pointer_v<T>)
            return Translate<std::remove_pointer_t<T>>(uint32_t(IntArg(ctx, gpr++, 4)));
        else
        {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "unsupported guest argument type");
            return T(IntArg(ctx, gpr++, sizeof(T)));
        }
    }

    template<typename T>
    void WriteResult(PPCContext& ctx, T value)
    {
        if constexpr (std::is_floating_point_v<T>)
            ctx.f1.f64 = double(value);
        else if constexpr (IsGuestPtr<T>::value)
            ctx.r3.u64 = value.address;
        else if constexpr (std::is_pointer_v<T>)
            ctx.r3.u64 = ToGuest(value);
        else if constexpr (std::is_signed_v<T>)
            ctx.r3.s64 = int64_t(value);
        else
            ctx.r3.u64 = uint64_t(value);
    }

    template<typename T> struct Decay { using type = std::decay_t<T>; };
    template<> struct Decay<PPCContext&> { using type = PPCContext&; };

    template<typename R, typename... Args>
    void InvokeHost(R (*func)(Args...), PPCContext& ctx)
    {
        size_t gpr = 0, fpr = 0;
        // Braced initialization evaluates the arguments left to right, matching register order.
        std::tuple<typename Decay<Args>::type...> args{ ReadArg<typename Decay<Args>::type>(ctx, gpr, fpr)... };
        if constexpr (std::is_void_v<R>)
            std::apply(func, args);
        else
            WriteResult(ctx, std::apply(func, args));
    }

    // Prints the guest call stack: always for CriticalSectionWait (long lock waits), otherwise when
    // PEGGLE2_BACKTRACE names the import (with import tracing on).
    void MaybeLogBacktrace(const char* importName);

    // Calls a guest function on the current thread's context. Integer and pointer arguments only.
    uint64_t GuestCall(uint32_t address, std::initializer_list<uint64_t> args = {});
}

#define GUEST_IMPORT(name, func)                                          \
    PPC_FUNC(__imp__##name)                                               \
    {                                                                     \
        if (!::rt::g_traceImports || !::rt::TraceImport(#name))           \
            return ::rt::InvokeHost(func, ctx);                           \
        uint32_t a0 = ctx.r3.u32, a1 = ctx.r4.u32, a2 = ctx.r5.u32, a3 = ctx.r6.u32; \
        ::rt::InvokeHost(func, ctx);                                      \
        ::rt::Log("{}({:X}, {:X}, {:X}, {:X}) = {:X} lr={:X}", #name,     \
            a0, a1, a2, a3, ctx.r3.u32, uint32_t(ctx.lr));                \
        ::rt::MaybeLogBacktrace(#name);                                   \
    }
