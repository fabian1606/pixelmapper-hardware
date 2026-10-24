idf_component_register(SRCS "src/pm_net.c" "src/pm_net_ip.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES pm_board pm_protocol esp_common
                       PRIV_REQUIRES esp_eth esp_netif lwip esp_event esp_timer freertos log)
