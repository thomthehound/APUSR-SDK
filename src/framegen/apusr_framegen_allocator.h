// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>

struct ApusrFramegenAllocator {
    using AllocFunc = void* (*)(void* userContext, std::uint64_t size);
    using DeallocFunc = void (*)(void* userContext, void* memory);

    void* userContext = nullptr;
    AllocFunc allocCallback = nullptr;
    DeallocFunc deallocCallback = nullptr;

    [[nodiscard]] void* alloc(std::size_t size) const noexcept
    {
        return allocCallback ? allocCallback(userContext, static_cast<std::uint64_t>(size)) : std::malloc(size);
    }

    void dealloc(void* memory) const noexcept
    {
        if (deallocCallback)
            deallocCallback(userContext, memory);
        else
            std::free(memory);
    }

    template <class T, class... Args>
    [[nodiscard]] T* construct(Args&&... args) const
    {
        void* memory = alloc(sizeof(T));
        return memory ? ::new (memory) T(std::forward<Args>(args)...) : nullptr;
    }

    template <class T>
    void destroy(T* object) const noexcept
    {
        if (!object)
            return;
        object->~T();
        dealloc(object);
    }
};
