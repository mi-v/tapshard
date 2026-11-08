cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(totp-token)

if(CONFIG_BUILD_OUTPUT_UF2)
  if(NOT CONFIG_TOTP_SECURITY_BOOTLOADER)
    message(FATAL_ERROR "UF2 requires the security bootloader/provisioning gate.")
  endif()
  if(NOT CONFIG_TOTP_UF2_BENCH AND
     (CONFIG_TOTP_DEBUG OR CONFIG_TOTP_TEST_POLL OR NOT CONFIG_NRF_APPROTECT_LOCK))
    message(FATAL_ERROR "Protected UF2 requires conf/uf2-production.conf.")
  endif()
  set(boot_output ${CMAKE_CURRENT_BINARY_DIR}/security-bootloader)
  set(boot_options)
  if(CONFIG_TOTP_UF2_BENCH)
    list(APPEND boot_options --unlocked)
  endif()
  add_custom_command(
    OUTPUT ${boot_output}/bootloader_payload.S ${boot_output}/bootloader.bin
    BYPRODUCTS ${boot_output}/bootloader.elf ${boot_output}/bootloader.map
      ${boot_output}/bootloader.json
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/run-limited.sh --seconds 90
      ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/uf2/build_bootloader.py
      --cc ${CMAKE_C_COMPILER} --objcopy ${CMAKE_OBJCOPY}
      --output ${boot_output} ${boot_options}
    DEPENDS tools/uf2/build_bootloader.py tools/run-limited.sh
      bootloader/startup.S bootloader/main.c bootloader/boot.c bootloader/boot.h
      bootloader/factory.c
      bootloader/reset_marker.h
      bootloader/sha256.c bootloader/sha256.h bootloader/svc.S
      bootloader/runtime.c bootloader/linker.ld
    VERBATIM)
  target_include_directories(app PRIVATE bootloader)
  target_sources(app PRIVATE src/uf2_layout.c src/boot_security.c
    bootloader/boot.c bootloader/factory.c bootloader/sha256.c bootloader/svc.S
    ${boot_output}/bootloader_payload.S)
  add_custom_target(totp_validate_uf2 ALL
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/run-limited.sh
      ${PYTHON_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/uf2/validate.py
      ${PROJECT_BINARY_DIR}/zephyr/zephyr.uf2
    DEPENDS zephyr_final
    VERBATIM)
endif()

target_sources(app PRIVATE
  src/main.c
  src/keyboard.c
  src/totp.c
  src/usb_msc.c
  src/usb_power_replay.c
  src/config.c
  src/config_fs.c
  src/profiles.c
  src/time.c
  src/button.c
)

zephyr_link_libraries(-Wl,--wrap=nrfx_power_usbevt_init)

if(CONFIG_TOTP_BATTERY_MONITOR)
  target_sources(app PRIVATE src/battery.c)
endif()
