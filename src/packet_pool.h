#pragma once
#include "common.h"
#include <cstddef>

inline constexpr std::size_t PACKET_POOL_SIZE = 4096;
inline constexpr std::size_t PACKET_BUFFER_SIZE = 4096;

void initPacketPool();
void cleanupPacketPool();
PacketNode *allocateNode(const char *buf, UINT len, const WINDIVERT_ADDRESS *addr);
void deallocateNode(PacketNode *node);
[[nodiscard]] bool isNodeFromPool(const PacketNode *node);
