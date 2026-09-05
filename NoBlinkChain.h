#pragma once

#include <cstdint>

namespace NoBlinkChain
{
    template <typename Node>
    constexpr Node* Detach(Node*& head)
    {
        Node* detached = head;
        head = nullptr;
        return detached;
    }

    template <typename Node, typename NextRef, typename IsValid>
    constexpr bool Append(Node*& head, Node* suffix, NextRef nextRef, IsValid isValid,
        const uint32_t maxWalk)
    {
        if (suffix == nullptr)
            return true;
        if (head == nullptr)
        {
            head = suffix;
            return true;
        }

        Node* node = head;
        for (uint32_t guard = 0; guard < maxWalk; ++guard)
        {
            if (!isValid(node))
                return false;
            Node*& next = nextRef(node);
            if (next == nullptr)
            {
                next = suffix;
                return true;
            }
            node = next;
        }
        return false;
    }
}
