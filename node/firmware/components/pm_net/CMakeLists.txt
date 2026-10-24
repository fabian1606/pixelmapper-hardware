idf_component_register(SRCS "src/pm_net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_board pm_protocol esp_common
                       PRIV_REQUIRES esp_eth esp_netif lwip esp_event)
