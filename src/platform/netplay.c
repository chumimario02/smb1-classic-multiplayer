#include "netplay.h"

#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_SDL2
#include <SDL.h>
#else
#include <SDL3/SDL.h>
#endif

#define RX_MAX 65536
#define TX_MAX 2048
#define TX_QUEUE 32
#define RELAY_QUEUE 32
#define RELAY_MAX 512

struct netplay {
  struct lws_context *context;
  struct lws *wsi;
  SDL_Thread *thread;
  volatile bool stop;

  char host[128];
  char path[256];
  int port;
  bool use_ssl;

  // Handshake data, kept so we can retry (Render free instances cold start).
  char join_name[32];
  char join_room[8];
  bool want_host;
  int attempts;
  uint32_t next_attempt_ms;

  bool connected;
  bool joined;
  bool is_host;
  bool has_peer;

  char room[8];
  char error[160];
  char relay_q[RELAY_QUEUE][RELAY_MAX];
  int relay_head;
  int relay_tail;

  char rx[RX_MAX];
  size_t rx_len;

  char tx[TX_QUEUE][TX_MAX];
  int tx_head;
  int tx_tail;
  SDL_mutex *lock;

  uint32_t last_ping_ms;
};

static int netplay_callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

static const struct lws_protocols protocols[] = {
  { "smb1-classic", netplay_callback, 0, 0, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static bool parse_server_url(struct netplay *np, const char *url) {
  const char *p = url;

  if (strncmp(p, "wss://", 6) == 0) {
    np->use_ssl = true;
    p += 6;
  } else if (strncmp(p, "ws://", 5) == 0) {
    np->use_ssl = false;
    p += 5;
  } else {
    snprintf(np->error, sizeof(np->error), "URL must start with ws:// or wss://");
    return false;
  }

  const char *slash = strchr(p, '/');
  const size_t full_host_len = slash ? (size_t)(slash - p) : strlen(p);
  if (full_host_len == 0 || full_host_len >= sizeof(np->host)) {
    snprintf(np->error, sizeof(np->error), "Invalid server host");
    return false;
  }

  const char *colon = memchr(p, ':', full_host_len);
  size_t host_len = full_host_len;

  if (colon) {
    host_len = (size_t)(colon - p);
    np->port = atoi(colon + 1);
  } else {
    np->port = np->use_ssl ? 443 : 80;
  }

  if (host_len == 0 || host_len >= sizeof(np->host) || np->port <= 0) {
    snprintf(np->error, sizeof(np->error), "Invalid server address");
    return false;
  }

  memcpy(np->host, p, host_len);
  np->host[host_len] = 0;

  if (slash) {
    snprintf(np->path, sizeof(np->path), "%s", slash);
  } else {
    snprintf(np->path, sizeof(np->path), "/");
  }

  return true;
}

static void clean_json(char *dst, size_t dstsz, const char *src) {
  size_t j = 0;

  for (size_t i = 0; src && src[i] && j + 1 < dstsz; i++) {
    const char c = src[i];
    if (c == '"' || c == '\\' || c == '\n' || c == '\r') {
      continue;
    }
    dst[j++] = c;
  }

  dst[j] = 0;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t outsz) {
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

  const char *p = strstr(json, pattern);
  if (!p) {
    return false;
  }

  p += strlen(pattern);

  size_t i = 0;
  while (*p && *p != '"' && i + 1 < outsz) {
    out[i++] = *p++;
  }
  out[i] = 0;

  return true;
}

static void set_error(struct netplay *np, const char *message) {
  snprintf(np->error, sizeof(np->error), "%s", message ? message : "network error");
}

static void netplay_send_text(struct netplay *np, const char *text) {
  if (!np->lock) {
    return;
  }

  SDL_LockMutex(np->lock);

  const int next = (np->tx_tail + 1) % TX_QUEUE;

  if (next == np->tx_head) {
    SDL_UnlockMutex(np->lock);
    return; // queue full; drop
  }

  snprintf(np->tx[np->tx_tail], TX_MAX, "%s", text);
  np->tx_tail = next;

  SDL_UnlockMutex(np->lock);

  if (np->wsi && np->connected) {
    lws_callback_on_writable(np->wsi);
    if (np->context) {
      lws_cancel_service(np->context);
    }
  }
}

static void netplay_process_message(struct netplay *np) {
  np->rx[np->rx_len] = 0;

  char type[32] = {0};
  char room[8] = {0};
  char error[160] = {0};

  if (!json_get_string(np->rx, "t", type, sizeof(type))) {
    np->rx_len = 0;
    return;
  }

  if (strcmp(type, "created") == 0) {
    np->joined = true;
    np->is_host = true;
    if (json_get_string(np->rx, "room", room, sizeof(room))) {
      snprintf(np->room, sizeof(np->room), "%s", room);
    }
  } else if (strcmp(type, "joined") == 0) {
    np->joined = true;
    np->is_host = false;
    if (json_get_string(np->rx, "room", room, sizeof(room))) {
      snprintf(np->room, sizeof(np->room), "%s", room);
    }
    np->has_peer = strstr(np->rx, "\"peers\":[{") != NULL;
  } else if (strcmp(type, "peer_joined") == 0) {
    np->has_peer = true;
  } else if (strcmp(type, "peer_left") == 0) {
    np->has_peer = false;
  } else if (strcmp(type, "relay") == 0) {
    np->has_peer = true;

    const char *data = strstr(np->rx, "\"data\":");
    if (data) {
      data += 7;

      char relay[RELAY_MAX];
      snprintf(relay, sizeof(relay), "%s", data);

      char *end = strrchr(relay, '}');
      if (end) {
        *end = 0;
      }

      SDL_LockMutex(np->lock);
      const int next = (np->relay_tail + 1) % RELAY_QUEUE;
      if (next != np->relay_head) {
        snprintf(np->relay_q[np->relay_tail], RELAY_MAX, "%s", relay);
        np->relay_tail = next;
      }
      SDL_UnlockMutex(np->lock);
    }
  } else if (strcmp(type, "room_closed") == 0) {
    np->joined = false;
    np->has_peer = false;
  } else if (strcmp(type, "error") == 0) {
    if (json_get_string(np->rx, "msg", error, sizeof(error))) {
      set_error(np, error);
    }
  }

  np->rx_len = 0;
}

static int netplay_callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
  struct netplay *np = user;

  if (!np) {
    return 0;
  }

  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    np->connected = true;
    np->wsi = wsi;
    np->attempts = 0;
    np->error[0] = 0;
    lws_callback_on_writable(wsi);
    break;

  case LWS_CALLBACK_CLIENT_WRITEABLE: {
    char text[TX_MAX];

    SDL_LockMutex(np->lock);
    const bool have = np->tx_head != np->tx_tail;
    if (have) {
      memcpy(text, np->tx[np->tx_head], TX_MAX);
      np->tx_head = (np->tx_head + 1) % TX_QUEUE;
    }
    const bool more = np->tx_head != np->tx_tail;
    SDL_UnlockMutex(np->lock);

    if (!have) {
      break;
    }

    const size_t text_len = strlen(text);
    unsigned char *buf = malloc(LWS_PRE + text_len);
    if (!buf) {
      break;
    }

    memcpy(buf + LWS_PRE, text, text_len);
    const int written = lws_write(wsi, buf + LWS_PRE, text_len, LWS_WRITE_TEXT);
    free(buf);

    if (more && written >= 0) {
      lws_callback_on_writable(wsi);
    }
    break;
  }

  case LWS_CALLBACK_CLIENT_RECEIVE:
    if (lws_frame_is_binary(wsi)) {
      break;
    }
    if (len > 0) {
      if (np->rx_len + len >= RX_MAX) {
        np->rx_len = 0;
      }
      memcpy(np->rx + np->rx_len, in, len);
      np->rx_len += len;
    }
    if (lws_is_final_fragment(wsi)) {
      netplay_process_message(np);
    }
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    np->connected = false;
    np->joined = false;
    np->has_peer = false;
    set_error(np, len > 0 && in ? (const char *)in : "connection error");
    break;

  case LWS_CALLBACK_CLIENT_CLOSED:
    np->connected = false;
    np->joined = false;
    np->has_peer = false;
    np->wsi = NULL;
    break;

  case LWS_CALLBACK_WSI_DESTROY:
    if (np->wsi == wsi) {
      np->wsi = NULL;
    }
    break;

  default:
    break;
  }

  return 0;
}

static void netplay_start(struct netplay *np) {
  np->attempts++;
  np->next_attempt_ms = SDL_GetTicks() + 3000;
  np->error[0] = 0;

  struct lws_client_connect_info ccinfo;
  memset(&ccinfo, 0, sizeof(ccinfo));
  ccinfo.context = np->context;
  ccinfo.address = np->host;
  ccinfo.port = np->port;
  ccinfo.path = np->path;
  ccinfo.host = np->host;
  ccinfo.origin = "origin";
  ccinfo.protocol = protocols[0].name;
  ccinfo.local_protocol_name = protocols[0].name;
  ccinfo.ssl_connection = np->use_ssl ? LCCSCF_USE_SSL : 0;
  ccinfo.userdata = np;
  ccinfo.pwsi = &np->wsi;

  np->wsi = lws_client_connect_via_info(&ccinfo);
  if (!np->wsi) {
    set_error(np, "could not start WebSocket connection");
    return;
  }

  char hello[128];
  snprintf(hello, sizeof(hello), "{\"t\":\"hello\",\"name\":\"%s\"}", np->join_name);
  netplay_send_text(np, hello);

  if (np->want_host) {
    char create[256];
    if (np->join_room[0]) {
      snprintf(create, sizeof(create),
               "{\"t\":\"create\",\"name\":\"1-1\",\"public\":false,\"max\":2,\"code\":\"%s\"}",
               np->join_room);
    } else {
      snprintf(create, sizeof(create),
               "{\"t\":\"create\",\"name\":\"1-1\",\"public\":false,\"max\":2}");
    }
    netplay_send_text(np, create);
  } else {
    char join[256];
    snprintf(join, sizeof(join),
             "{\"t\":\"join\",\"room\":\"%s\",\"name\":\"%s\"}",
             np->join_room, np->join_name);
    netplay_send_text(np, join);
  }
}

static int netplay_thread(void *data) {
  struct netplay *np = data;

  while (!np->stop) {
    lws_service(np->context, 5);

    // Retry while the server is unreachable (Render free instances sleep and
    // need ~50 seconds to wake up).
    if (!np->connected && np->attempts < 40 &&
        SDL_GetTicks() >= np->next_attempt_ms) {
      netplay_start(np);
    }
  }

  return 0;
}

struct netplay *netplay_create(const struct netplay_config *cfg) {
  struct netplay *np = calloc(1, sizeof(*np));
  if (!np) {
    return NULL;
  }

  np->lock = SDL_CreateMutex();
  if (!np->lock) {
    set_error(np, "could not create network mutex");
    return np;
  }

  if (!parse_server_url(np, cfg->server_url ? cfg->server_url : "ws://127.0.0.1:8080")) {
    return np; // error is stored; context is NULL
  }

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.user = np;

  np->context = lws_create_context(&info);
  if (!np->context) {
    set_error(np, "could not create WebSocket context");
    return np;
  }

  clean_json(np->join_name, sizeof(np->join_name), cfg->name ? cfg->name : "PLAYER");
  np->want_host = cfg->host;
  if (cfg->host) {
    if (cfg->room && strlen(cfg->room) == 4) {
      clean_json(np->join_room, sizeof(np->join_room), cfg->room);
    }
  } else {
    clean_json(np->join_room, sizeof(np->join_room), cfg->room ? cfg->room : "");
  }

  netplay_start(np);

  np->thread = SDL_CreateThread(netplay_thread, "netplay", np);
  if (!np->thread) {
    set_error(np, "could not start network thread");
  }

  return np;
}

void netplay_destroy(struct netplay *np) {
  if (!np) {
    return;
  }

  if (np->thread) {
    np->stop = true;
    if (np->context) {
      lws_cancel_service(np->context);
    }
    SDL_WaitThread(np->thread, NULL);
    np->thread = NULL;
  }

  if (np->context) {
    lws_context_destroy(np->context);
  }

  if (np->lock) {
    SDL_DestroyMutex(np->lock);
  }

  free(np);
}

void netplay_update(struct netplay *np, uint32_t now_ms) {
  // The WebSocket service runs on its own thread.
  (void)np;
  (void)now_ms;
}

bool netplay_connected(const struct netplay *np) {
  return np && np->connected;
}

bool netplay_joined(const struct netplay *np) {
  return np && np->joined;
}

bool netplay_has_peer(const struct netplay *np) {
  return np && np->has_peer;
}

const char *netplay_room(const struct netplay *np) {
  return np && np->room[0] ? np->room : NULL;
}

const char *netplay_error(const struct netplay *np) {
  return np && np->error[0] ? np->error : NULL;
}

bool netplay_get_relay(struct netplay *np, uint32_t *seq, char *out, size_t outsz) {
  (void)seq;

  if (!np || !np->lock) {
    return false;
  }

  bool got = false;

  SDL_LockMutex(np->lock);
  if (np->relay_head != np->relay_tail) {
    snprintf(out, outsz, "%s", np->relay_q[np->relay_head]);
    np->relay_head = (np->relay_head + 1) % RELAY_QUEUE;
    got = true;
  }
  SDL_UnlockMutex(np->lock);

  return got;
}

void netplay_send_relay(struct netplay *np, const char *json_data) {
  if (!np || !np->connected || !np->joined) {
    return;
  }

  char msg[TX_MAX];
  snprintf(msg, sizeof(msg), "{\"t\":\"relay\",\"data\":%s}",
           json_data && json_data[0] ? json_data : "{}");
  netplay_send_text(np, msg);
}
