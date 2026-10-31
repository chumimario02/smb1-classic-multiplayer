#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdbool.h>
#include <stdint.h>

struct netplay;

struct netplay_config {
  const char *server_url;
  const char *room;
  const char *name;
  bool host;
};

struct netplay *netplay_create(const struct netplay_config *cfg);
void netplay_destroy(struct netplay *np);

// Service the WebSocket connection. Call once per frame.
void netplay_update(struct netplay *np, uint32_t now_ms);

bool netplay_connected(const struct netplay *np);
bool netplay_joined(const struct netplay *np);
bool netplay_has_peer(const struct netplay *np);
const char *netplay_room(const struct netplay *np);
const char *netplay_error(const struct netplay *np);
uint32_t netplay_relay_seq(const struct netplay *np);
const char *netplay_last_relay(const struct netplay *np);

// Send a generic relay payload (JSON object) to the other player.
void netplay_send_relay(struct netplay *np, const char *json_data);

#endif
