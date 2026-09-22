#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>

#include "packet_pool.h"

namespace
{
class PacketPool final
{
public:
    void initialize()
    {
        std::scoped_lock lock(mutex_);
        if (initialized_)
            return;

        resetUnlocked();
        initialized_ = true;
    }

    void reset()
    {
        std::scoped_lock lock(mutex_);
        resetUnlocked();
        initialized_ = false;
    }

    PacketNode *allocate(const char *buffer, UINT length, const WINDIVERT_ADDRESS *address)
    {
        if (!buffer || !address || length == 0 || length > PACKET_BUFFER_SIZE)
            return nullptr;

        std::scoped_lock lock(mutex_);
        if (!initialized_ || freeCount_ == 0)
            return nullptr;

        const std::size_t index = freeStack_[--freeCount_];
        used_[index] = true;

        PacketNode &node = nodes_[index];
        std::memcpy(node.packet, buffer, length);
        node.packetLen = length;
        node.addr = *address;
        node.timestamp = 0;
        node.prev = nullptr;
        node.next = nullptr;
        return &node;
    }

    void deallocate(PacketNode *node)
    {
        const auto index = indexOf(node);
        if (!index)
            return;

        std::scoped_lock lock(mutex_);
        if (!initialized_ || !used_[*index])
            return;

        used_[*index] = false;
        node->packetLen = 0;
        node->timestamp = 0;
        node->prev = nullptr;
        node->next = nullptr;
        if (freeCount_ < PACKET_POOL_SIZE)
            freeStack_[freeCount_++] = *index;
    }

    [[nodiscard]] bool owns(const PacketNode *node) const noexcept
    {
        return indexOf(node).has_value();
    }

private:
    [[nodiscard]] std::optional<std::size_t> indexOf(const PacketNode *node) const noexcept
    {
        if (!node)
            return {};

        const auto address = reinterpret_cast<std::uintptr_t>(node);
        const auto begin = reinterpret_cast<std::uintptr_t>(nodes_.data());
        const auto end = begin + sizeof(nodes_);
        if (address < begin || address >= end)
            return {};

        const auto offset = address - begin;
        if (offset % sizeof(PacketNode) != 0)
            return {};

        return static_cast<std::size_t>(offset / sizeof(PacketNode));
    }

    void resetUnlocked() noexcept
    {
        used_.fill(false);
        freeCount_ = PACKET_POOL_SIZE;
        for (std::size_t index = 0; index < PACKET_POOL_SIZE; ++index)
        {
            nodes_[index] = {};
            nodes_[index].packet = buffers_[index].data();
            freeStack_[index] = index;
        }
    }

    std::array<PacketNode, PACKET_POOL_SIZE> nodes_{};
    std::array<std::array<char, PACKET_BUFFER_SIZE>, PACKET_POOL_SIZE> buffers_{};
    std::array<std::size_t, PACKET_POOL_SIZE> freeStack_{};
    std::array<bool, PACKET_POOL_SIZE> used_{};
    std::size_t freeCount_ = 0;
    bool initialized_ = false;
    mutable std::mutex mutex_;
};

PacketPool g_pool;
} // namespace

void initPacketPool() { g_pool.initialize(); }
void cleanupPacketPool() { g_pool.reset(); }

PacketNode *allocateNode(const char *buffer, UINT length, const WINDIVERT_ADDRESS *address)
{
    return g_pool.allocate(buffer, length, address);
}

void deallocateNode(PacketNode *node) { g_pool.deallocate(node); }

bool isNodeFromPool(const PacketNode *node) { return g_pool.owns(node); }
