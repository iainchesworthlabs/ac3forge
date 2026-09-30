#pragma once

// lwIP's IPv4 input hook for iclforge/tcp_arrivals.hpp, which dates the bytes
// of the Sendspin player's connections as they arrive. lwIP includes this
// file through ESP_IDF_LWIP_HOOK_FILENAME, which a project sets on the lwip
// component after project() (ESP-IDF's lwIP guide, "Customized lwIP Hooks"):
//
//   idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
//   idf_component_get_property(iclforge_dir iclforge COMPONENT_DIR)
//   target_compile_options(${lwip_lib} PRIVATE "-I${iclforge_dir}/lwip_hooks")
//   target_compile_definitions(${lwip_lib} PRIVATE
//       "ESP_IDF_LWIP_HOOK_FILENAME=\"iclforge_lwip_hooks.h\"")
//
// Without it the player dates a message by when its task reads it.

struct pbuf;
struct netif;

#ifdef __cplusplus
extern "C" {
#endif

int iclforge_lwip_ip4_input(struct pbuf *p, struct netif *inp);

#ifdef __cplusplus
}
#endif

#define LWIP_HOOK_IP4_INPUT(p, inp) iclforge_lwip_ip4_input((p), (inp))
