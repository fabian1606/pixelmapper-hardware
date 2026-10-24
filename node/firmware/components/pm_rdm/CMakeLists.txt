# pure C11, no ESP-IDF includes (also built by test_host)
idf_component_register(SRCS "src/pm_rdm.c" "src/pm_rdm_ctrl.c" "src/pm_rdm_responder.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pm_protocol)
