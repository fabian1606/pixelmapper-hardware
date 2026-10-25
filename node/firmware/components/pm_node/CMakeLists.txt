# pure C11, no ESP-IDF includes (also built by main/test_host and usable by a host "virtual node")
idf_component_register(SRCS "src/pm_node.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_protocol pm_artnet pm_rdm)
