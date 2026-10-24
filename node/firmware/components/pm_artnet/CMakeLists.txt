# pure C11, no ESP-IDF includes (also built by test_host)
idf_component_register(SRCS "src/pm_artnet_packets.c" "src/pm_artnet_node.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_protocol)
