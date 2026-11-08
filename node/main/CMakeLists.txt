idf_component_register(
    SRCS "rbgrid_node_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer esp_driver_i2c esp_driver_gpio rbamp mbedtls
)

# Role select: default SLAVE (node). Build the test beacon source with -DRBN_ROLE_MASTER=1.
if(RBN_ROLE_MASTER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBN_ROLE_MASTER=1)
endif()

# Free-running node (beacon-less OPEN receivers like ACRouter): broadcast OPEN RT on a fixed channel, no hub
# beacon/PTP/pairing/encryption. Build with -DRBN_FREERUN=1 (optionally -DTEST_CHANNEL=N, default 1).
if(RBN_FREERUN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBN_FREERUN=1)
endif()
if(DEFINED TEST_CHANNEL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TEST_CHANNEL=${TEST_CHANNEL})
endif()

# Output node (wireless dimmer/relay AT the load, driven by the hub via SET_OUTPUT 0x40). This build = a
# GPIO-relay reference node, OPEN bring-up. Build -DRBN_OUTPUT_NODE=1 (optionally -DRBN_OUT_GPIO=N, default 2).
if(RBN_OUTPUT_NODE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBN_OUTPUT_NODE=1)
endif()
if(DEFINED RBN_OUT_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBN_OUT_GPIO=${RBN_OUT_GPIO})
endif()
# Output actuator kind: relay (default) or DimmerLink dimmer over I2C. -DRBN_OUT_IS_DIMMER=1 for the dimmer node.
if(RBN_OUT_IS_DIMMER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBN_OUT_IS_DIMMER=1)
endif()
