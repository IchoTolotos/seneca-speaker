idf_component_register(
    SRCS "airplay_tx.cpp" "vendor/raop_AnySpeaker.cpp" "vendor/plist_bin.c" "vendor/crypto/curve25519_donna.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "compat" "vendor"
    REQUIRES esp_common esp_timer freertos
    PRIV_REQUIRES lwip mbedtls nvs_flash esp_netif espressif__mdns esphome__libsodium)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function -Wno-missing-field-initializers
                       -Wno-deprecated-declarations -Wno-format-truncation -Wno-stringop-truncation -Wno-unused-but-set-variable)

# esphome/libsodium is a verification-only subset; AirPlay 2 pairing also needs Ed25519 key generation
# and signing. Compile those upstream libsodium files here (same sources, same version as the component).
idf_component_get_property(SODIUM_DIR esphome__libsodium COMPONENT_DIR)
set(SODIUM_SIGN
    "${SODIUM_DIR}/libsodium/src/libsodium/crypto_sign/crypto_sign.c"
    "${SODIUM_DIR}/libsodium/src/libsodium/crypto_sign/ed25519/sign_ed25519.c"
    "${SODIUM_DIR}/libsodium/src/libsodium/crypto_sign/ed25519/ref10/keypair.c"
    "${SODIUM_DIR}/libsodium/src/libsodium/crypto_sign/ed25519/ref10/sign.c")
target_sources(${COMPONENT_LIB} PRIVATE ${SODIUM_SIGN})
target_include_directories(${COMPONENT_LIB} PRIVATE "${SODIUM_DIR}/libsodium/src/libsodium/include/sodium")
# the component already provides _crypto_sign_ed25519_ref10_hinit (port/ed25519_ref10_hinit.c)
set_source_files_properties("${SODIUM_DIR}/libsodium/src/libsodium/crypto_sign/ed25519/ref10/sign.c"
    PROPERTIES COMPILE_OPTIONS "-D_crypto_sign_ed25519_ref10_hinit=gls_unused_sodium_hinit;-DCONFIGURED=1;-Wno-unused-function")
set_source_files_properties(${SODIUM_SIGN} PROPERTIES COMPILE_DEFINITIONS "CONFIGURED=1")
