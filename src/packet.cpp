#include <cstring>
#include <memory>
#include <new>

#include "common.h"
#include "packet_pool.h"

static PacketNode headNode{}, tailNode{};
extern PacketNode *const head = &headNode;
extern PacketNode *const tail = &tailNode;

void initPacketNodeList()
{
    initPacketPool();
    if (head->next == NULL && tail->prev == NULL)
    {

        head->next = tail;
        tail->prev = head;
    }
    else
    {
        while (!isListEmpty())
        {
            PacketNode *node = head->next;
            popNode(node);
            freeNode(node);
        }
    }
}

PacketNode *createNode(const char *buf, UINT len, const WINDIVERT_ADDRESS *addr)
{
    PacketNode *node = allocateNode(buf, len, addr);
    if (node)
        return node;

    if (!buf || len == 0 || len > 0xFFFF || !addr)
    {
        return NULL;
    }

    auto newNode = std::unique_ptr<PacketNode>(new (std::nothrow) PacketNode{});
    if (!newNode)
        return nullptr;

    char *rawPacket = new (std::nothrow) char[len];
    if (rawPacket == nullptr)
        return nullptr;
    auto packet = std::unique_ptr<char[]>(rawPacket);

    std::memcpy(rawPacket, buf, len);
    newNode->packet = packet.release();
    newNode->packetLen = len;
    newNode->addr = *addr;
    newNode->next = newNode->prev = nullptr;
    newNode->timestamp = 0;
    return newNode.release();
}

void freeNode(PacketNode *node)
{
    if (!node || node == head || node == tail)
        return;

    if (isNodeFromPool(node))
    {
        deallocateNode(node);
    }
    else
    {
        delete[] node->packet;
        delete node;
    }
}

PacketNode *popNode(PacketNode *node)
{
    if (!node || node == head || node == tail || !node->prev || !node->next)
        return nullptr;
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = nullptr;
    node->next = nullptr;
    return node;
}

PacketNode *insertAfter(PacketNode *node, PacketNode *target)
{
    if (!node || node == head || node == tail || !target || target == tail || !target->next)
        return nullptr;
    node->prev = target;
    node->next = target->next;
    target->next->prev = node;
    target->next = node;
    return node;
}

PacketNode *insertBefore(PacketNode *node, PacketNode *target)
{
    if (!node || node == head || node == tail || !target || target == head || !target->prev)
        return nullptr;
    node->next = target;
    node->prev = target->prev;
    target->prev->next = node;
    target->prev = node;
    return node;
}

PacketNode *appendNode(PacketNode *node) { return insertBefore(node, tail); }

short isListEmpty() { return head->next == tail; }
