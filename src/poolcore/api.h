#include <stddef.h>
#include <stdint.h>

class p2pPeer;

void poolcoreRequestHandler(p2pPeer *peer, uint64_t id, void *buffer, size_t size, void *arg);
